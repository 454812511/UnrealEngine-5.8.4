// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DCC/MetaHumanCharacterDCCExport.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterEyes.h"
#include "Subsystem/MetaHumanCharacterSkinMaterials.h"
#include "Tests/MetaHumanCharacterTestUtils.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

//----------------------------------------------------------------------
// GetEyelashesMask unit tests
//----------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FGetEyelashesMaskTest, "MetaHumanCreator.DCCExport.GetEyelashesMask", EAutomationTestFlags::EngineFilter | EAutomationTestFlags::EditorContext)
END_DEFINE_SPEC(FGetEyelashesMaskTest)

void FGetEyelashesMaskTest::Define()
{
	Describe("GetEyelashesMask", [this]()
	{
		It("should return a non-null texture for every non-None eyelashes type", [this]()
		{
			for (EMetaHumanCharacterEyelashesType EyelashesType : TEnumRange<EMetaHumanCharacterEyelashesType>())
			{
				if (EyelashesType == EMetaHumanCharacterEyelashesType::None)
				{
					continue;
				}
				FMetaHumanCharacterEyelashesProperties Properties;
				Properties.Type = EyelashesType;
				UTexture2D* Mask = FMetaHumanCharacterSkinMaterials::GetEyelashesMask(Properties);
				const FString TypeName = StaticEnum<EMetaHumanCharacterEyelashesType>()->GetAuthoredNameStringByValue(static_cast<int64>(EyelashesType));
				TestNotNull(FString::Printf(TEXT("Eyelashes mask for type %s"), *TypeName), Mask);
			}
		});

		It("should return an empty mask for None", [this]()
		{
			FMetaHumanCharacterEyelashesProperties NoneProperties;
			NoneProperties.Type = EMetaHumanCharacterEyelashesType::None;
			UTexture2D* Mask = FMetaHumanCharacterSkinMaterials::GetEyelashesMask(NoneProperties);
			// None returns the empty mask placeholder, which is still a valid (non-null) texture object
			TestNotNull(TEXT("Empty mask for None is still a valid texture"), Mask);
		});

		It("should return distinct textures for different eyelashes types", [this]()
		{
			FMetaHumanCharacterEyelashesProperties SparseProps;
			SparseProps.Type = EMetaHumanCharacterEyelashesType::ShortSparse;
			UTexture2D* ShortSparse = FMetaHumanCharacterSkinMaterials::GetEyelashesMask(SparseProps);

			FMetaHumanCharacterEyelashesProperties CurlProps;
			CurlProps.Type = EMetaHumanCharacterEyelashesType::LongCurl;
			UTexture2D* LongCurl = FMetaHumanCharacterSkinMaterials::GetEyelashesMask(CurlProps);

			FMetaHumanCharacterEyelashesProperties NoneProps;
			NoneProps.Type = EMetaHumanCharacterEyelashesType::None;
			UTexture2D* None = FMetaHumanCharacterSkinMaterials::GetEyelashesMask(NoneProps);

			TestNotEqual(TEXT("ShortSparse and LongCurl should return different textures"), ShortSparse, LongCurl);
			TestNotEqual(TEXT("ShortSparse and None should return different textures"), ShortSparse, None);
		});
	});
}

//----------------------------------------------------------------------
// DCC Export integration tests
//----------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FDCCExportTest, "MetaHumanCreator.DCCExport.ExportCharacterForDCC", EAutomationTestFlags::EngineFilter | EAutomationTestFlags::EditorContext)

	/** Temp directory used by the current test run; cleaned up in AfterEach. */
	FString OutputFolder;

	/**
	 * Initialize a character from the Ada preset via the editor subsystem,
	 * returning the character ready for export. Returns an empty TStrongObjectPtr on failure.
	 */
	TStrongObjectPtr<UMetaHumanCharacter> CreateCharacterFromPreset()
	{
		UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
		if (!Subsystem)
		{
			return nullptr;
		}

		TStrongObjectPtr<UMetaHumanCharacter> Character(NewObject<UMetaHumanCharacter>(GetTransientPackage()));
		Subsystem->InitializeMetaHumanCharacter(Character.Get());

		if (!Subsystem->TryAddObjectToEdit(Character.Get()))
		{
			return nullptr;
		}

		UMetaHumanCharacter* Preset = LoadObject<UMetaHumanCharacter>(nullptr, TEXT("/" UE_PLUGIN_NAME "/Optional/Presets/Ada.Ada"));
		if (!Preset)
		{
			Subsystem->RemoveObjectToEdit(Character.Get());
			return nullptr;
		}

		Subsystem->InitializeFromPreset(Character.Get(), Preset);

		// Set up the TDS exchange code handler (from command line) so the blocking cloud requests below can authenticate
		TUniquePtr<UE::MetaHuman::TDSUtils::FScopedExchangeCodeHandler> ScopedExchangeHandler = UE::MetaHuman::TestUtils::MakeTDSScopedExchangeCodeHandler();

		// Rig the character (blocking) so it is ready for DCC export
		FMetaHumanCharacterAutoRiggingRequestParams RiggingParams;
		RiggingParams.bBlocking = true;
		RiggingParams.bReportProgress = false;
		Subsystem->RequestAutoRigging(Character.Get(), RiggingParams);

		// Request high resolution textures (blocking) so they are available for DCC export
		FMetaHumanCharacterTextureRequestParams TextureParams;
		TextureParams.bBlocking = true;
		TextureParams.bReportProgress = false;
		Subsystem->RequestTextureSources(Character.Get(), TextureParams);

		return Character;
	}

	/** Check that a file exists under the character output folder. */
	bool DoesExportedFileExist(const FString& CharacterName, const FString& RelativePath) const
	{
		const FString FullPath = OutputFolder / CharacterName / RelativePath;
		return FPaths::FileExists(FullPath);
	}

	/** Load the exported manifest JSON into the struct. Returns true on success. */
	bool LoadExportManifest(const FString& CharacterName, FMetaHumanExportDCCManifest& OutManifest) const
	{
		const FString ManifestPath = OutputFolder / CharacterName / TEXT("ExportManifest.json");
		FString JsonString;
		if (!FFileHelper::LoadFileToString(JsonString, *ManifestPath))
		{
			return false;
		}
		return FJsonObjectConverter::JsonObjectStringToUStruct(JsonString, &OutManifest);
	}

	/** Load an exported image file (relative to the character output folder) into an FImage. Returns true on success. */
	bool LoadExportedImage(const FString& CharacterName, const FString& RelativePath, FImage& OutImage) const
	{
		const FString FullPath = OutputFolder / CharacterName / RelativePath;
		return FImageUtils::LoadImage(*FullPath, OutImage);
	}

END_DEFINE_SPEC(FDCCExportTest)

void FDCCExportTest::Define()
{
	BeforeEach([this]()
	{
		OutputFolder = FPaths::ProjectSavedDir() / TEXT("DCCExportTest") / FGuid::NewGuid().ToString();
	});

	AfterEach([this]()
	{
		// Clean up the temp directory tree
		if (!OutputFolder.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*OutputFolder, /*RequireExists*/ false, /*Tree*/ true);
		}
	});

	Describe("Export preconditions", [this]()
	{
		It("should not produce any output when the character is not rigged", [this]()
		{
			UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
			TestNotNull(TEXT("Editor subsystem"), Subsystem);
			if (!Subsystem)
			{
				return;
			}

			// Create a character but do NOT initialize from a preset so it remains unrigged
			TStrongObjectPtr<UMetaHumanCharacter> Character(NewObject<UMetaHumanCharacter>(GetTransientPackage()));
			Subsystem->InitializeMetaHumanCharacter(Character.Get());
			TestTrue(TEXT("Character was added to edit"), Subsystem->TryAddObjectToEdit(Character.Get()));
			ON_SCOPE_EXIT
			{
				Subsystem->RemoveObjectToEdit(Character.Get());
			};

			TestTrue(TEXT("Character is not rigged"), Subsystem->GetRiggingState(Character.Get()) != EMetaHumanCharacterRigState::Rigged);

			AddExpectedErrorPlain(TEXT("MetaHuman Character DCC export failed"), EAutomationExpectedErrorFlags::Contains, 2);

			FMetaHumanCharacterEditorDCCExportParameters ExportParams;
			ExportParams.OutputFolderPath = OutputFolder;
			ExportParams.bBakeFaceMakeup = true;
			ExportParams.bExportZipFile = false;
			FMetaHumanCharacterEditorDCCExport::ExportCharacterForDCC(Character.Get(), ExportParams);

			// The export should have bailed before writing anything
			const FString CharacterFolder = OutputFolder / Character->GetName();
			TestFalse(TEXT("No output folder was created for unrigged character"), FPaths::DirectoryExists(CharacterFolder));
		});
	});

	Describe("Eyelashes export with selected type", [this]()
	{
		It("should produce Eyelashes_Color.png in the Maps folder when eyelashes type is set", [this]()
		{
			TStrongObjectPtr<UMetaHumanCharacter> Character = CreateCharacterFromPreset();
			TestNotNull(TEXT("Character created from preset"), Character.Get());
			if (!Character.IsValid())
			{
				return;
			}
			UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
			ON_SCOPE_EXIT
			{
				Subsystem->RemoveObjectToEdit(Character.Get());
			};

			if (!TestTrue(TEXT("Character is rigged"), Subsystem->GetRiggingState(Character.Get()) == EMetaHumanCharacterRigState::Rigged)
				|| !TestTrue(TEXT("Character has high resolution textures"), Character->HasHighResolutionTextures()))
			{
				return;
			}

			Character->HeadModelSettings.Eyelashes.Type = EMetaHumanCharacterEyelashesType::LongCurl;

			FMetaHumanCharacterEditorDCCExportParameters ExportParams;
			ExportParams.OutputFolderPath = OutputFolder;
			ExportParams.bBakeFaceMakeup = true;
			ExportParams.bExportZipFile = false;
			FMetaHumanCharacterEditorDCCExport::ExportCharacterForDCC(Character.Get(), ExportParams);

			const FString CharacterName = Character->GetName();

			// The manifest should exist
			FMetaHumanExportDCCManifest Manifest;
			const bool bManifestLoaded = LoadExportManifest(CharacterName, Manifest);
			TestTrue(TEXT("Export manifest was written"), bManifestLoaded);

			if (bManifestLoaded)
			{
				// The manifest Maps list must contain the eyelashes entry
				TestTrue(TEXT("Manifest contains Eyelashes_Color.png"), Manifest.Files.Maps.Contains(TEXT("Eyelashes_Color.png")));
			}

			// The actual file should exist on disk
			TestTrue(TEXT("Eyelashes_Color.png exists on disk"), DoesExportedFileExist(CharacterName, TEXT("Maps/Eyelashes_Color.png")));

			// Clean up temp folder and verify it was removed
			IFileManager::Get().DeleteDirectory(*OutputFolder, /*RequireExists*/ false, /*Tree*/ true);
			TestFalse(TEXT("Temp output folder was deleted"), FPaths::DirectoryExists(OutputFolder));
		});
	});

	Describe("Eyelashes export with None type", [this]()
	{
		It("should still produce Eyelashes_Color.png when eyelashes type is None", [this]()
		{
			TStrongObjectPtr<UMetaHumanCharacter> Character = CreateCharacterFromPreset();
			TestNotNull(TEXT("Character created from preset"), Character.Get());
			if (!Character.IsValid())
			{
				return;
			}
			UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
			ON_SCOPE_EXIT
			{
				Subsystem->RemoveObjectToEdit(Character.Get());
			};

			if (!TestTrue(TEXT("Character is rigged"), Subsystem->GetRiggingState(Character.Get()) == EMetaHumanCharacterRigState::Rigged)
				|| !TestTrue(TEXT("Character has high resolution textures"), Character->HasHighResolutionTextures()))
			{
				return;
			}

			// Set eyelashes to None — the export should still produce a mask using the fallback
			Character->HeadModelSettings.Eyelashes.Type = EMetaHumanCharacterEyelashesType::None;

			FMetaHumanCharacterEditorDCCExportParameters ExportParams;
			ExportParams.OutputFolderPath = OutputFolder;
			ExportParams.bBakeFaceMakeup = true;
			ExportParams.bExportZipFile = false;
			FMetaHumanCharacterEditorDCCExport::ExportCharacterForDCC(Character.Get(), ExportParams);

			const FString CharacterName = Character->GetName();

			// Even with None, the manifest should contain the eyelashes entry
			FMetaHumanExportDCCManifest Manifest;
			const bool bManifestLoaded = LoadExportManifest(CharacterName, Manifest);
			TestTrue(TEXT("Export manifest was written"), bManifestLoaded);

			if (bManifestLoaded)
			{
				TestTrue(TEXT("Manifest contains Eyelashes_Color.png even when eyelashes type is None"), Manifest.Files.Maps.Contains(TEXT("Eyelashes_Color.png")));
			}

			// The actual file should exist on disk
			TestTrue(TEXT("Eyelashes_Color.png exists on disk even when eyelashes type is None"), DoesExportedFileExist(CharacterName, TEXT("Maps/Eyelashes_Color.png")));

			// Clean up temp folder and verify it was removed
			IFileManager::Get().DeleteDirectory(*OutputFolder, /*RequireExists*/ false, /*Tree*/ true);
			TestFalse(TEXT("Temp output folder was deleted"), FPaths::DirectoryExists(OutputFolder));
		});
	});

	Describe("Export completeness", [this]()
	{
		It("should produce all expected files and folders", [this]()
		{
			TStrongObjectPtr<UMetaHumanCharacter> Character = CreateCharacterFromPreset();
			TestNotNull(TEXT("Character created from preset"), Character.Get());
			if (!Character.IsValid())
			{
				return;
			}
			UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
			ON_SCOPE_EXIT
			{
				Subsystem->RemoveObjectToEdit(Character.Get());
			};

			if (!TestTrue(TEXT("Character is rigged"), Subsystem->GetRiggingState(Character.Get()) == EMetaHumanCharacterRigState::Rigged)
				|| !TestTrue(TEXT("Character has high resolution textures"), Character->HasHighResolutionTextures()))
			{
				return;
			}

			FMetaHumanCharacterEditorDCCExportParameters ExportParams;
			ExportParams.OutputFolderPath = OutputFolder;
			ExportParams.bBakeFaceMakeup = true;
			ExportParams.bExportZipFile = false;
			FMetaHumanCharacterEditorDCCExport::ExportCharacterForDCC(Character.Get(), ExportParams);

			const FString CharacterName = Character->GetName();
			const FString CharacterFolder = OutputFolder / CharacterName;

			// ---- Manifest ----
			FMetaHumanExportDCCManifest Manifest;
			const bool bManifestLoaded = LoadExportManifest(CharacterName, Manifest);
			TestTrue(TEXT("Export manifest was written"), bManifestLoaded);

			if (!bManifestLoaded)
			{
				return;
			}

			// Basic manifest metadata
			TestFalse(TEXT("MetaHumanName is not empty"), Manifest.MetaHumanName.IsEmpty());
			TestFalse(TEXT("ExportPluginVersion is not empty"), Manifest.ExportPluginVersion.IsEmpty());
			TestFalse(TEXT("ExportEngineVersion is not empty"), Manifest.ExportEngineVersion.IsEmpty());

			// ---- Thumbnail ----
			TestFalse(TEXT("Manifest Thumbnail field is set"), Manifest.Thumbnail.IsEmpty());
			if (!Manifest.Thumbnail.IsEmpty())
			{
				TestTrue(
					FString::Printf(TEXT("Thumbnail file %s exists on disk"), *Manifest.Thumbnail),
					DoesExportedFileExist(CharacterName, Manifest.Thumbnail));
			}

			// ---- DNA files ----
			TestFalse(TEXT("Manifest Dna.Head is set"), Manifest.Dna.Head.IsEmpty());
			if (!Manifest.Dna.Head.IsEmpty())
			{
				TestTrue(
					FString::Printf(TEXT("Head DNA file %s exists on disk"), *Manifest.Dna.Head),
					DoesExportedFileExist(CharacterName, Manifest.Dna.Head));
			}

			TestFalse(TEXT("Manifest Dna.Body is set"), Manifest.Dna.Body.IsEmpty());
			if (!Manifest.Dna.Body.IsEmpty())
			{
				TestTrue(
					FString::Printf(TEXT("Body DNA file %s exists on disk"), *Manifest.Dna.Body),
					DoesExportedFileExist(CharacterName, Manifest.Dna.Body));
			}

			// ---- Maps folder and files ----
			const FString MapsFolderPath = CharacterFolder / Manifest.Folders.Maps;
			TestTrue(TEXT("Maps folder exists on disk"), FPaths::DirectoryExists(MapsFolderPath));
			TestTrue(TEXT("Maps list is not empty"), Manifest.Files.Maps.Num() > 0);
			TestTrue(TEXT("Maps contains Eyelashes_Color.png"), Manifest.Files.Maps.Contains(TEXT("Eyelashes_Color.png")));

			for (const FString& MapFile : Manifest.Files.Maps)
			{
				TestTrue(
					FString::Printf(TEXT("Maps file %s exists on disk"), *MapFile),
					DoesExportedFileExist(CharacterName, Manifest.Folders.Maps / MapFile));
			}

			// The exported eyelashes map must decode to an RGBA (4-channel) format with an alpha channel:
			// the grayscale strand data is packed into the alpha channel over an RGB tint, so a grayscale
			// (G8) export would mean the alpha channel was lost. A grayscale PNG decodes to ERawImageFormat::G8,
			// whereas an RGBA PNG decodes to ERawImageFormat::BGRA8.
			FImage EyelashesImage;
			const bool bEyelashesImageLoaded = LoadExportedImage(CharacterName, Manifest.Folders.Maps / TEXT("Eyelashes_Color.png"), EyelashesImage);
			TestTrue(TEXT("Eyelashes_Color.png could be loaded from disk"), bEyelashesImageLoaded);
			if (bEyelashesImageLoaded)
			{
				TestEqual(TEXT("Eyelashes_Color.png is in BGRA8 (RGBA) format"), static_cast<int32>(EyelashesImage.Format), static_cast<int32>(ERawImageFormat::BGRA8));
				TestTrue(TEXT("Eyelashes_Color.png has an alpha channel"), ERawImageFormat::HasAlphaChannel(EyelashesImage.Format));
			}

			// ---- Masks folder and files ----
			const FString MasksFolderPath = CharacterFolder / Manifest.Folders.Masks;
			TestTrue(TEXT("Masks folder exists on disk"), FPaths::DirectoryExists(MasksFolderPath));
			TestTrue(TEXT("Masks list is not empty"), Manifest.Files.Masks.Num() > 0);

			for (const FString& MaskFile : Manifest.Files.Masks)
			{
				TestTrue(
					FString::Printf(TEXT("Masks file %s exists on disk"), *MaskFile),
					DoesExportedFileExist(CharacterName, Manifest.Folders.Masks / MaskFile));
			}

			// Clean up temp folder and verify it was removed
			IFileManager::Get().DeleteDirectory(*OutputFolder, /*RequireExists*/ false, /*Tree*/ true);
			TestFalse(TEXT("Temp output folder was deleted"), FPaths::DirectoryExists(OutputFolder));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
