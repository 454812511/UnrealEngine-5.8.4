// Copyright Epic Games, Inc. All Rights Reserved.

#include "InterchangeDnaModule.h"
#include "MetaHumanInterchangeDnaTranslator.h"
#include "MetaHumanSDKSettings.h"

#include "InterchangeManager.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericMeshPipeline.h"

#include "SkelMeshDNAUtils.h"
#include "DNAToSkelMeshMap.h"
#include "SkelMeshDNAReader.h"
#include "DNAUtils.h"
#include "DNA.h"
#include "DNAAssetUserData.h"

#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopedSlowTask.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/AssetUserData.h"
#include "Engine/Engine.h"
#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsAssetUtils.h"
#include "Interfaces/IPluginManager.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserMenuContexts.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "ToolMenu.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FDNAInterchangeModule"

DEFINE_LOG_CATEGORY_STATIC(LogInterchangeDNA, Log, All);

namespace UE::InterchangeDNA::Private
{
	// Returns true if any skeletal mesh OTHER than InSkelMesh references the asset identified by the
	// given AssetRegistry tag (e.g. "Skeleton" or "PhysicsAsset") whose value is the asset's export name.
	// Used as a blast-radius guard: a from-scratch in-place rebuild of a shared companion would invalidate
	// the other meshes that point at it, so we skip the rebuild in that case.
	static bool IsCompanionSharedByAnotherMesh(USkeletalMesh* InSkelMesh, const FName InTagName, const UObject* InCompanion, FString& OutOtherMeshPath)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FARFilter Filter;
		Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
		Filter.TagsAndValues.Add(InTagName, FAssetData(InCompanion).GetExportTextName());

		TArray<FAssetData> SkeletalMeshAssetData;
		AssetRegistry.GetAssets(Filter, SkeletalMeshAssetData);

		const FSoftObjectPath ThisMeshPath(InSkelMesh);
		for (const FAssetData& AssetData : SkeletalMeshAssetData)
		{
			if (AssetData.GetSoftObjectPath() != ThisMeshPath)
			{
				OutOtherMeshPath = AssetData.GetSoftObjectPath().ToString();
				return true;
			}
		}
		return false;
	}
}

// Rebuild the bone tree and purge
// orphaned morph-target curves in place (preserving the USkeleton identity so external references stay
// valid and see the regenerated data). Skipped when the skeleton is shared with another skeletal mesh.
static void RegenerateSkeletonFromScratch(USkeletalMesh* InSkelMesh)
{
	if (!InSkelMesh)
	{
		return;
	}

	USkeleton* Skeleton = InSkelMesh->GetSkeleton();
	if (!Skeleton)
	{
		return;
	}

	FString OtherMeshPath;
	if (UE::InterchangeDNA::Private::IsCompanionSharedByAnotherMesh(InSkelMesh, TEXT("Skeleton"), Skeleton, OtherMeshPath))
	{
		UE_LOGFMT(LogInterchangeDNA, Warning,
			"DNA Generate: skeleton '{0}' is shared by another skeletal mesh ('{1}'); skipping from-scratch "
			"regeneration to avoid invalidating its bindings. Stale joints/curves may persist.",
			*Skeleton->GetName(), *OtherMeshPath);
		return;
	}

	// Bones: empty BoneTree + ReferenceSkeleton, regenerate the GUID, and re-merge from the fresh mesh.
	Skeleton->RecreateBoneTree(InSkelMesh);

	// Morph-target curve metadata: remove any morph target curve that is no longer backed by a morph target
	// on the freshly imported mesh (AddCurveMetaData on import is additive and never removes stale entries).
	TSet<FName> ValidMorphCurveNames;
	for (const TObjectPtr<UMorphTarget>& MorphTarget : InSkelMesh->GetMorphTargets())
	{
		if (MorphTarget)
		{
			ValidMorphCurveNames.Add(MorphTarget->GetFName());
		}
	}

	TArray<FName> AllCurveNames;
	Skeleton->GetCurveMetaDataNames(AllCurveNames);

	TArray<FName> StaleMorphCurveNames;
	for (const FName& CurveName : AllCurveNames)
	{
		if (Skeleton->GetCurveMetaDataMorphTarget(CurveName) && !ValidMorphCurveNames.Contains(CurveName))
		{
			StaleMorphCurveNames.Add(CurveName);
		}
	}

	if (StaleMorphCurveNames.Num() > 0)
	{
		Skeleton->RemoveCurveMetaData(TArrayView<FName>(StaleMorphCurveNames));
	}
}

// On second Generate the existing PhysicsAsset is reused and its bodies/constraints are NOT
// regenerated (the factory gates that work behind !bIsReImport), so they keep referencing stale bones.
// Rebuild it in place from the freshly imported mesh (preserving the UPhysicsAsset identity so external
// references stay valid). Skipped when the PhysicsAsset is shared with another skeletal mesh.
static void RegeneratePhysicsAssetFromScratch(USkeletalMesh* InSkelMesh)
{
	if (!InSkelMesh)
	{
		return;
	}

	UPhysicsAsset* PhysicsAsset = InSkelMesh->GetPhysicsAsset();
	if (!PhysicsAsset)
	{
		return;
	}

	FString OtherMeshPath;
	if (UE::InterchangeDNA::Private::IsCompanionSharedByAnotherMesh(InSkelMesh, TEXT("PhysicsAsset"), PhysicsAsset, OtherMeshPath))
	{
		UE_LOGFMT(LogInterchangeDNA, Warning,
			"DNA Generate: PhysicsAsset '{0}' is shared by another skeletal mesh ('{1}'); skipping from-scratch "
			"regeneration to avoid invalidating its bodies/constraints. Stale bodies may persist.",
			*PhysicsAsset->GetName(), *OtherMeshPath);
		return;
	}

	// FPhysicsAssetUtils::CreateFromSkeletalMesh APPENDS bodies/constraints (it assumes a fresh, empty
	// asset), so clear the existing setup first to avoid accumulating duplicate/stale bodies. It clears
	// CollisionDisableTable and rebuilds BoundsBodies itself, so we only empty the body/constraint arrays.
	PhysicsAsset->SkeletalBodySetups.Empty();
	PhysicsAsset->ConstraintSetup.Empty();
	PhysicsAsset->UpdateBodySetupIndexMap();

	FPhysAssetCreateParams Params;
	FText ErrorMessage;
	const bool bSuccess = FPhysicsAssetUtils::CreateFromSkeletalMesh(
		PhysicsAsset, InSkelMesh, Params, ErrorMessage,
		/*bSetToMesh=*/false,
		/*bShowProgress=*/false);
	if (!bSuccess)
	{
		UE_LOGFMT(LogInterchangeDNA, Error,
			"DNA Generate: failed to regenerate PhysicsAsset '{0}' from mesh '{1}': {2}",
			*PhysicsAsset->GetName(), *InSkelMesh->GetName(), *ErrorMessage.ToString());
		return;
	}

	// CreateFromSkeletalMesh mutates the body/constraint arrays by raw assignment (no PostEditChange), so
	// the package's dirty flag never flips, mark it explicitly and nudge any open Phat editor to re-bind.
	PhysicsAsset->MarkPackageDirty();
	PhysicsAsset->RefreshPhysicsAssetChange();
}


namespace MenuExtention_DNAAsset
{
	static void ExecuteGenerateSkelMesh(const UContentBrowserAssetContextMenuContext* InCBContext)
	{
		UDNA* DNAAsset = Cast<UDNA>(InCBContext->SelectedAssets[0].GetAsset());

		if (!DNAAsset)
		{
			return;
		}

		// We have to check if MHSDK plugin is enabled
		TSharedPtr<IPlugin> MetaHumanSDKPlugin = IPluginManager::Get().FindPlugin(TEXT("MetaHumanSDK"));
		if (!MetaHumanSDKPlugin.IsValid() || !MetaHumanSDKPlugin->IsEnabled())
		{
			const FText Message = LOCTEXT("DNAGenerateSkelMeshRequiresMetaHumanSDK", "Generate Skeletal Mesh requires MetaHumanSDK plugin to be enabled.");
			FMessageDialog::Open(EAppMsgType::Ok, Message);
			return;
		}

		FScopedSlowTask SlowTask(100.0f, LOCTEXT("GeneratingSkeletalMesh", "Generating Skeletal Mesh from DNA..."));
		SlowTask.MakeDialog();

		SlowTask.EnterProgressFrame(10.0f);

		TSharedPtr<IDNAReader> DNAReader = DNAAsset->GetDNAReader();
		if (!DNAReader)
		{
			UE_LOGFMT(LogInterchangeDNA, Error, "Failed to get DNA reader from DNA asset");
			return;
		}

		SlowTask.EnterProgressFrame(20.0f);

		FString TempDnaPath = FPaths::CreateTempFilename(
			FPlatformProcess::UserTempDir(),
			TEXT("DNA_"), TEXT(".mhdna"));

		WriteDNAToFile(DNAReader.Get(), EDNADataLayer::All, TempDnaPath);

		SlowTask.EnterProgressFrame(20.0f);

		UInterchangeSourceData* SourceData = NewObject<UInterchangeSourceData>();
		SourceData->SetFilename(TempDnaPath);

		UDNAConfigHolder* DNAConfigHolder = NewObject<UDNAConfigHolder>(SourceData);
		DNAConfigHolder->Config = DNAAsset->DNAConfig;
		SourceData->SetContextObjectByTag(TEXT("DNAConfig"), DNAConfigHolder);

		UInterchangeManager& InterchangeManager = UInterchangeManager::GetInterchangeManager();

		FImportAssetParameters ImportAssetParameters;
		ImportAssetParameters.bIsAutomated = true;
		ImportAssetParameters.bFollowRedirectors = false;
		ImportAssetParameters.ReimportAsset = nullptr;
		ImportAssetParameters.bReplaceExisting = true;

		FString AssetName = DNAAsset->GetName() + TEXT("_SKM");
		FString AssetPath = FPackageName::GetLongPackagePath(DNAAsset->GetPathName());
		ImportAssetParameters.DestinationName = AssetName;

		const UMetaHumanSDKSettings* Settings = GetDefault<UMetaHumanSDKSettings>();
		FSoftObjectPath PipelinePath = Settings->DefaultDNAPipelinePath;

		UInterchangeGenericAssetsPipeline* PipeAsset = Cast<UInterchangeGenericAssetsPipeline>(PipelinePath.TryLoad());

		if (PipeAsset)
		{
			PipeAsset->CommonSkeletalMeshesAndAnimationsProperties->Skeleton = nullptr;
			PipeAsset->CommonSkeletalMeshesAndAnimationsProperties->bAddCurveMetadataToSkeleton = true;

			FSoftObjectPath AssetPipeline = FSoftObjectPath(PipeAsset);
			ImportAssetParameters.OverridePipelines.Add(AssetPipeline);
		}

		SlowTask.EnterProgressFrame(50.0f);

		UE::Interchange::FAssetImportResultRef ImportResult = InterchangeManager.ImportAssetWithResult(AssetPath, SourceData, ImportAssetParameters);

		// Using a weakptr here since lambda callback may come after GC collects DNAAsset
		TWeakObjectPtr<UDNA> WeakDNAAsset = DNAAsset;

		ImportResult->OnDone([TempDnaPath, WeakDNAAsset](UE::Interchange::FImportResult& Result)
			{
				IFileManager::Get().Delete(*TempDnaPath);

				if (Result.GetImportedObjects().Num() > 0)
				{
					for (UObject* ImportedObject : Result.GetImportedObjects())
					{
						if (USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(ImportedObject))
						{
							// Rebuild the reused
							// skeleton (bones + curve metadata) and PhysicsAsset in place to purge stale data.
							RegenerateSkeletonFromScratch(SkelMesh);
							RegeneratePhysicsAssetFromScratch(SkelMesh);

							UDNAAssetUserData* DNAUserData = NewObject<UDNAAssetUserData>(SkelMesh);

							if(UDNA* DNAAsset = WeakDNAAsset.Get())
							{
								DNAUserData->DNAAsset = DNAAsset;
							}
							else
							{
								const FText Message = LOCTEXT("DNA_FailedToAttach", "Failed to attach DNA to generated Skeletal Mesh");
								UE_LOGFMT(LogInterchangeDNA, Warning, "{}", *Message.ToString());
							}

							SkelMesh->AddAssetUserData(DNAUserData);
							SkelMesh->GetPackage()->MarkPackageDirty();

							FNotificationInfo Info(FText::Format(LOCTEXT("DNA_SkelMeshGenSuccess", "Skeletal Mesh '{0}' generated successfully!"), FText::FromString(SkelMesh->GetName())));
							Info.ExpireDuration = 5.0f;
							Info.bFireAndForget = true;
							TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
							if (Notification.IsValid())
							{
								Notification->SetCompletionState(SNotificationItem::CS_Success);
							}
							break;
						}
					}
				}
				else
				{
					FNotificationInfo Info(LOCTEXT("DNA_SkelMeshGenFailed", "Failed to generate Skeletal Mesh"));
					Info.ExpireDuration = 5.0f;
					Info.bFireAndForget = true;
					TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
					if (Notification.IsValid())
					{
						Notification->SetCompletionState(SNotificationItem::CS_Fail);
					}
				}
			});
	}

	static void ExtendDNAAssetActions()
	{
		FToolMenuOwnerScoped OwnderScoped(UE_MODULE_NAME);
		{
			UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UDNA::StaticClass())
				->AddDynamicSection(
					NAME_None,
					FNewToolMenuDelegate::CreateLambda(
						[](UToolMenu* InMenu)
						{
							const UContentBrowserAssetContextMenuContext* Context = InMenu->FindContext<UContentBrowserAssetContextMenuContext>();
							if (Context && Context->SelectedAssets.Num() > 0)
							{
								FToolMenuSection& Section = InMenu->FindOrAddSection(TEXT("GetAssetActions"));
								{
									{
										const FText Label = LOCTEXT("DNAAssetGenerateSKM", "Generate Skeletal Mesh");
										const FText Tooltip = LOCTEXT("DNAAssetGenerateSKMTooltip", "Generate Skeletal Mesh from this DNA.");
										const FSlateIcon Icon{ FAppStyle::GetAppStyleSetName(), "Icons.SkeletalMesh" };
										const FUIAction UIAction = FUIAction(
											FExecuteAction::CreateStatic(&ExecuteGenerateSkelMesh, Context)
										);
										Section.AddMenuEntry(TEXT("DNAAssetGenerateSKM"), Label, Tooltip, Icon, UIAction);
									}
								}
							}
						}
					)
				);
		}
	}


	static FDelayedAutoRegisterHelper DelayedAutoRegister(
		EDelayedRegisterRunPhase::EndOfEngineInit,
		[]
		{
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&ExtendDNAAssetActions));
		}
	);
}

void FInterchangeDnaModule::StartupModule()
{
	auto RegisterItems = []()
	{
		UInterchangeManager& InterchangeManager = UInterchangeManager::GetInterchangeManager();

		// Register DNA translator here. In this way we don't have to change Project Settings. 
		// Interchange manager will recognize DNA file extension and run the translator overriding existing DNA Factory.
		InterchangeManager.RegisterTranslator(UMetaHumanInterchangeDnaTranslator::StaticClass());
	};

	if (GEngine)
	{
		RegisterItems();
	}
	else
	{
		FCoreDelegates::GetOnPostEngineInit().AddLambda(RegisterItems);
	}

	UInterchangeManager::SetInterchangeImportEnabled(true);
}

void FInterchangeDnaModule::ShutdownModule()
{
	UInterchangeManager::SetInterchangeImportEnabled(false);
}

FInterchangeDnaModule& FInterchangeDnaModule::GetModule()
{
	static const FName ModuleName = UE_MODULE_NAME;
	return FModuleManager::LoadModuleChecked<FInterchangeDnaModule>(ModuleName);
}

void FInterchangeDnaModule::SetSkelMeshDNAData(TNotNull<USkeletalMesh*> InSkelMesh, TSharedPtr<IDNAReader> InDNAReader)
{
	TObjectPtr<UDNAAsset> DNAAsset = NewObject<UDNAAsset>(InSkelMesh);
	DNAAsset->SetDNAReader(InDNAReader, EDNACopyPolicy::Alias, ERigLogicInitPolicy::Defer);
	DNAAsset->RestoreLegacyUEMHCCompatibility();
	if (DNAAsset)
	{
		InSkelMesh->AddAssetUserData(DNAAsset);
	}
	
	const TSharedRef<FDNAToSkelMeshMap> FaceDnaToSkelMeshMap = MakeShareable(USkelMeshDNAUtils::CreateMapForUpdatingNeutralMesh(InSkelMesh));
	FaceDnaToSkelMeshMap->MapJoints(InDNAReader.Get());
}

void FInterchangeDnaModule::CreateAndAttachDNAToSkeletalMesh(TNotNull<class USkeletalMesh*> InSkelMesh, TSharedPtr<IDNAReader> InDNAReader)
{
	if (!InDNAReader)
	{
		return;
	}

	FString DNAAssetName = InSkelMesh->GetName() + TEXT("_DNA");

	UPackage* DNAPackage = nullptr;
	UDNA* DNAAsset = nullptr;

	// Check if skeletal mesh is in a transient package
	if (InSkelMesh->GetPackage()->HasAnyFlags(RF_Transient))
	{
		DNAPackage = GetTransientPackage();
		DNAAsset = NewObject<UDNA>(DNAPackage, *DNAAssetName, RF_Transient);
	}
	else
	{
		FString PackagePath = FPackageName::GetLongPackagePath(InSkelMesh->GetPathName());
		FString DNAPackageName = PackagePath + TEXT("/") + DNAAssetName;
		FString DNAFullObjectPath = DNAPackageName + TEXT(".") + DNAAssetName;

		DNAAsset = LoadObject<UDNA>(nullptr, *DNAFullObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);

		if (!DNAAsset)
		{
			DNAPackage = CreatePackage(*DNAPackageName);
			DNAAsset = NewObject<UDNA>(DNAPackage, *DNAAssetName, RF_Public | RF_Standalone);
		}
	}

	if (!DNAAsset)
	{
		return;
	}

	DNAAsset->SetDNAReader(InDNAReader, EDNACopyPolicy::Alias, ERigLogicInitPolicy::Defer);
	DNAAsset->RestoreLegacyUEMHCCompatibility();

	UDNAAssetUserData* DNAUserData = NewObject<UDNAAssetUserData>(InSkelMesh);
	DNAUserData->DNAAsset = DNAAsset;

	InSkelMesh->AddAssetUserData(DNAUserData);

	if (DNAPackage && !DNAPackage->HasAnyFlags(RF_Transient))
	{
		DNAPackage->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(DNAAsset);
	}

	InSkelMesh->GetPackage()->MarkPackageDirty();
}

USkeletalMesh* FInterchangeDnaModule::ImportSync(const FString& InNewRigAssetName, const FString& InNewRigPath, TSharedPtr<IDNAReader> InDNAReader, TSoftObjectPtr<USkeleton> InSkeleton, const FDNAConfig& DNAConfig)
{
	USkeletalMesh* ImportedMesh = nullptr;

	if (InDNAReader)
	{
		// TODO: Since there is no support for memory stream yet in the Interchange SourceData system we need to create temporary file.
		FString DnaTempPath = FPaths::CreateTempFilename(
			FPlatformProcess::UserTempDir(),
			*InDNAReader->GetName(), TEXT(".mhdna"));

		WriteDNAToFile(InDNAReader.Get(), EDNADataLayer::All, DnaTempPath);

		if (FPaths::FileExists(DnaTempPath))
		{
			UE::Interchange::FScopedSourceData ScopedSourceData(DnaTempPath);
			UInterchangeSourceData* SourceData = ScopedSourceData.GetSourceData();
			UDNAConfigHolder* DNAConfigHolder = NewObject<UDNAConfigHolder>(SourceData);
			DNAConfigHolder->Config = DNAConfig;
			SourceData->SetContextObjectByTag(TEXT("DNAConfig"), DNAConfigHolder);

			FImportAssetParameters ImportAssetParameters;
			ImportAssetParameters.bIsAutomated = true;
			ImportAssetParameters.bFollowRedirectors = false;
			ImportAssetParameters.ReimportAsset = nullptr;
			ImportAssetParameters.bReplaceExisting = true;
			ImportAssetParameters.DestinationName = InNewRigAssetName;

			const UMetaHumanSDKSettings* Settings = GetDefault<UMetaHumanSDKSettings>();
			FSoftObjectPath PipelinePath = Settings->DefaultDNAPipelinePath;

			UInterchangeGenericAssetsPipeline* PipeAsset = Cast<UInterchangeGenericAssetsPipeline>(PipelinePath.TryLoad());

			if(!PipeAsset)
			{
				const FText Message = LOCTEXT("InterchangeDNAImportPipeAssetMissing", "Failed to get Interchange Asset Pipeline for DNA to SKM conversion.");
				UE_LOGFMT(LogInterchangeDNA, Error, "{}", *Message.ToString());
				return nullptr;
			}

			PipeAsset->MeshPipeline->bCreatePhysicsAsset = false;

			if (!InSkeleton.IsNull() && InSkeleton.LoadSynchronous() != nullptr)
			{
				PipeAsset->CommonSkeletalMeshesAndAnimationsProperties->Skeleton = InSkeleton;
				PipeAsset->CommonSkeletalMeshesAndAnimationsProperties->bAddCurveMetadataToSkeleton = false;
			}

			FSoftObjectPath AssetPipeline = FSoftObjectPath(PipeAsset);
			ImportAssetParameters.OverridePipelines.Add(AssetPipeline);
			UInterchangeManager& InterchangeManager = UInterchangeManager::GetInterchangeManager();

			UE::Interchange::FAssetImportResultRef ImportRes = InterchangeManager.ImportAssetWithResult(
				InNewRigPath, ScopedSourceData.GetSourceData(), ImportAssetParameters);

			for (UObject* Object : ImportRes->GetImportedObjects())
			{
				if (USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(Object))
				{
					ImportedMesh = SkelMesh;
					break;
				}
			}

			// Delete temporary file.
			IFileManager::Get().Delete(*DnaTempPath);
		}
	}

	return ImportedMesh;
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FInterchangeDnaModule, InterchangeDNA)
