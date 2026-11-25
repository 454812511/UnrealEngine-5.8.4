// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "IMovieSceneModule.h"
#include "MovieScene.h"
#include "Compilation/IMovieSceneTemplateGenerator.h"
#include "Evaluation/IMovieScenePlaybackCapability.h"
#include "EntitySystem/MovieSceneSharedPlaybackState.h"
#include "IMovieScenePlaybackClient.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "HAL/IConsoleManager.h"
#include "Logging/MessageLog.h"
#include "Misc/CoreDelegates.h"
#include "Misc/UObjectToken.h"
#include "Modules/VisualizerDebuggingState.h"
#include "EntitySystem/BuiltInComponentTypes.h"
#include "EntitySystem/MovieSceneEntityManager.h"
#include "EntitySystem/MovieSceneEntitySystem.h"
#include "Decorations/MovieSceneDecorationContainer.h"

DEFINE_LOG_CATEGORY(LogMovieScene);
DEFINE_LOG_CATEGORY(LogMovieSceneECS);

namespace UE::MovieScene
{

/**
 * Clears the IMovieScenePlaybackClient capability on shared playback states whose client can no longer be
 * resolved.
 *
 * The capability holds its client as a raw, non-owning pointer, so each UObject-backed client passed to
 * Track is also recorded weakly here. The check runs from reachability analysis, so a capability is cleared
 * at the first garbage collection after its weak record stops resolving. That is at or before the point the
 * client's memory is released, which is what matters: between an explicit destroy and that collection the
 * capability still points at a destroyed but intact client, which is safe to read. Clearing is one-way -
 * the record goes with it, and a state only regains a tracked client by being registered again.
 *
 * Clients that are not UObjects are ignored, having no collection event to guard against.
 *
 * Game thread only, matching playback capabilities.
 */
class FPlaybackClientTracker
{
public:

	FPlaybackClientTracker()
	{
		PostReachabilityAnalysisHandle = FCoreUObjectDelegates::PostReachabilityAnalysis.AddRaw(this, &FPlaybackClientTracker::ClearCapabilitiesForDestroyedClients);
	}

	~FPlaybackClientTracker()
	{
		FCoreUObjectDelegates::PostReachabilityAnalysis.Remove(PostReachabilityAnalysisHandle);
	}

	FPlaybackClientTracker(const FPlaybackClientTracker&) = delete;
	FPlaybackClientTracker& operator=(const FPlaybackClientTracker&) = delete;

	/** Records the client that InSharedPlaybackState's capability has just been given. */
	void Track(const TSharedRef<FSharedPlaybackState>& InSharedPlaybackState, IMovieScenePlaybackClient* InClient)
	{
		UObject* ClientObject = Cast<UObject>(InClient);
		if (!ClientObject)
		{
			// Not a UObject, so it is not garbage collected and has no collection event to guard against.
			return;
		}

		// A state holds at most one client capability, so replace any record for it rather than appending.
		// Dead records are dropped at the same time, which keeps this bounded between reachability passes
		// even when root instances are rebuilt repeatedly.
		const FSharedPlaybackState* PlaybackStateToTrack = &InSharedPlaybackState.Get();
		for (int32 Index = TrackedClients.Num() - 1; Index >= 0; --Index)
		{
			TSharedPtr<FSharedPlaybackState> ExistingPlaybackState = TrackedClients[Index].WeakPlaybackState.Pin();
			if (!ExistingPlaybackState.IsValid() || ExistingPlaybackState.Get() == PlaybackStateToTrack)
			{
				TrackedClients.RemoveAtSwap(Index);
			}
		}

		FTrackedClient& Tracked = TrackedClients.AddDefaulted_GetRef();
		Tracked.WeakPlaybackState = InSharedPlaybackState;
		Tracked.WeakClientObject = ClientObject;
		Tracked.ExpectedClient = InClient;
	}

private:

	void ClearCapabilitiesForDestroyedClients()
	{
		for (int32 Index = TrackedClients.Num() - 1; Index >= 0; --Index)
		{
			const FTrackedClient& Tracked = TrackedClients[Index];

			TSharedPtr<FSharedPlaybackState> SharedPlaybackState = Tracked.WeakPlaybackState.Pin();
			if (!SharedPlaybackState.IsValid() || SharedPlaybackState->FindCapability<IMovieScenePlaybackClient>() != Tracked.ExpectedClient)
			{
				// The state is gone, or something repointed the capability and we no longer know what it holds.
				TrackedClients.RemoveAtSwap(Index);
				continue;
			}

			if (!Tracked.WeakClientObject.IsValid())
			{
				SharedPlaybackState->GetCapabilities().OverwriteCapabilityRaw<IMovieScenePlaybackClient>(nullptr);
				TrackedClients.RemoveAtSwap(Index);
			}
		}
	}

	struct FTrackedClient
	{
		TWeakPtr<FSharedPlaybackState> WeakPlaybackState;
		TWeakObjectPtr<UObject> WeakClientObject;

		/** Compared to detect the capability having been repointed elsewhere. Never dereferenced. */
		const IMovieScenePlaybackClient* ExpectedClient = nullptr;
	};

	TArray<FTrackedClient> TrackedClients;
	FDelegateHandle PostReachabilityAnalysisHandle;
};

} // namespace UE::MovieScene

TAutoConsoleVariable<FString> CVarLegacyConversionFrameRate(
	TEXT("MovieScene.LegacyConversionFrameRate"),
	TEXT("60000fps"),
	TEXT("Specifies default tick resolution for UMovieScene data saved before 4.20 (default: 60000fps). Examples: 60000 fps, 120/1 (120 fps), 30000/1001 (29.97), 0.01s (10ms)."),
	ECVF_Default);

struct FCachedLegacyConversionFrameRate
{
	FCachedLegacyConversionFrameRate()
		: FrameRate(60000, 1)
	{
		IConsoleManager::Get().RegisterConsoleVariableSink_Handle(FConsoleCommandDelegate::CreateRaw(this, &FCachedLegacyConversionFrameRate::OnChanged));
		OnChanged();
	}

	void OnChanged()
	{
		TryParseString(FrameRate, *CVarLegacyConversionFrameRate.GetValueOnAnyThread());
	}

	FFrameRate FrameRate;
};

FFrameRate GetLegacyConversionFrameRate()
{
	static FCachedLegacyConversionFrameRate CachedRate;
	return CachedRate.FrameRate;
}

void EmitLegacyOutOfBoundsError(UObject* Object, FFrameRate InFrameRate, double InTime)
{
#if WITH_EDITOR
	static const FName NAME_AssetCheck("AssetCheck");

	FMessageLog AssetCheckLog(NAME_AssetCheck);

	const FText Message = FText::Format(
		NSLOCTEXT("MovieScene", "LegacyOutOfBoundsError", "Encountered time ({0} seconds) that is out of the supported range with a resolution of {1}fps. Saving this asset will cause loss of data. Please reduce MovieScene.LegacyConversionFrameRate and re-load this asset."),
		InTime, InFrameRate.AsDecimal()
	);
	AssetCheckLog.Error()
		->AddToken(FUObjectToken::Create(Object))
		->AddToken(FTextToken::Create(Message));

	AssetCheckLog.Open(EMessageSeverity::Warning);
#endif
}

FFrameNumber UpgradeLegacyMovieSceneTime(UObject* ErrorContext, FFrameRate InFrameRate, double InTime)
{
	double ClampedKeyTime = FMath::Clamp(InTime, -InFrameRate.MaxSeconds(), InFrameRate.MaxSeconds());
	if (InTime != ClampedKeyTime)
	{
		EmitLegacyOutOfBoundsError(ErrorContext, InFrameRate, InTime);
	}
	return InFrameRate.AsFrameNumber(ClampedKeyTime);
}

/**
 * MovieScene module implementation.
 */
class FMovieSceneModule
	: public IMovieSceneModule
	, public TSharedFromThis<FMovieSceneModule>
{
public:

	// IModuleInterface interface
	~FMovieSceneModule()
	{
		ensure(ModuleHandle.IsUnique());
	}

	virtual void StartupModule() override
	{
		using namespace UE::Core;
		using namespace UE::MovieScene;

		FModuleManager::Get().LoadModuleChecked("UniversalObjectLocator");

		// Register the global debug ptrs
		FGuid GEntityManagerForDebuggingGuid(0xaf15add3, 0xd76b4d2c, 0xa79d574b, 0xb4cd9706);
		EVisualizerDebuggingStateResult Result1 = FVisualizerDebuggingState::Assign(GEntityManagerForDebuggingGuid, &GEntityManagerForDebuggingVisualizers);

		FGuid GPlaybackCapabilityIDRegistryForDebuggingGuid(0xf9062bb9, 0xd77c4bc7, 0x861e5d87, 0xd5afb22d);
		EVisualizerDebuggingStateResult Result2 = FVisualizerDebuggingState::Assign(GPlaybackCapabilityIDRegistryForDebuggingGuid, &GPlaybackCapabilityIDRegistryForDebuggingVisualizers);
		GPlaybackCapabilityIDRegistryForDebuggingVisualizers = FPlaybackCapabilityIDRegistry::Get();

		struct FNoopDefaultDeleter
		{
			void operator()(FMovieSceneModule* Object) const {}
		};
		ModuleHandle = MakeShareable(this, FNoopDefaultDeleter());

		FBuiltInComponentTypes::Get();

		PlaybackClientTracker = MakeUnique<FPlaybackClientTracker>();

		FCoreDelegates::OnAllModuleLoadingPhasesComplete.AddStatic(&UMovieSceneEntitySystem::StaticInitialize);
	}

	virtual void ShutdownModule() override
	{
		PlaybackClientTracker.Reset();

		UE::MovieScene::FBuiltInComponentTypes::Destroy();
	}

	void TrackPlaybackClient(const TSharedRef<UE::MovieScene::FSharedPlaybackState>& InSharedPlaybackState, IMovieScenePlaybackClient* InClient)
	{
		if (PlaybackClientTracker)
		{
			PlaybackClientTracker->Track(InSharedPlaybackState, InClient);
		}
	}

	virtual void RegisterEvaluationGroupParameters(FName GroupName, const FMovieSceneEvaluationGroupParameters& GroupParameters) override
	{
		check(!GroupName.IsNone() && GroupParameters.EvaluationPriority != 0);

		for (auto& Pair : EvaluationGroupParameters)
		{
			checkf(Pair.Key != GroupName, TEXT("Cannot add 2 groups of the same name"));
			checkf(Pair.Value.EvaluationPriority != GroupParameters.EvaluationPriority, TEXT("Cannot add 2 groups of the same priority"));
		}

		EvaluationGroupParameters.Add(GroupName, GroupParameters);
	}

	virtual FMovieSceneEvaluationGroupParameters GetEvaluationGroupParameters(FName GroupName) const override
	{
		return EvaluationGroupParameters.FindRef(GroupName);
	}

	virtual TWeakPtr<IMovieSceneModule> GetWeakPtr() override
	{
		return ModuleHandle;
	}
	
	virtual void RegisterCompatibleDecoration(TSubclassOf<UMovieSceneDecorationContainerObject> ContainerClass, TSubclassOf<UObject> DecorationClass) override
	{
		if (!ContainerClass || !DecorationClass)
		{
			return;
		}

		TSet<TSubclassOf<UObject>>& Decorations = CompatibleDecorations.FindOrAdd(ContainerClass);
		Decorations.Add(DecorationClass);
	}

	virtual void UnregisterCompatibleDecoration(TSubclassOf<UMovieSceneDecorationContainerObject> ContainerClass, TSubclassOf<UObject> DecorationClass) override
	{
		// If the engine is exiting, do not do anything as data may have already been trashed before this call.
		if (IsEngineExitRequested())
		{
			return;
		}

		if (!ContainerClass || !DecorationClass)
		{
			return;
		}

		if (TSet<TSubclassOf<UObject>>* Decorations = CompatibleDecorations.Find(ContainerClass))
		{
			Decorations->Remove(DecorationClass);
			if (Decorations->IsEmpty())
			{
				CompatibleDecorations.Remove(ContainerClass);
			}
		}
	}

	virtual void GetCompatibleDecorationsForContainer(const UMovieSceneDecorationContainerObject* Container, TSet<UClass*>& OutClasses) const override
	{
		if (!Container)
		{
			return;
		}

		// First get the decorations from the container's own implementation
		Container->GetCompatibleUserDecorations(OutClasses);

		// Then add any registered decorations for this class and its parent classes
		UClass* ContainerClass = Container->GetClass();
		while (ContainerClass && ContainerClass->IsChildOf(UMovieSceneDecorationContainerObject::StaticClass()))
		{
			if (const TSet<TSubclassOf<UObject>>* RegisteredDecorations = CompatibleDecorations.Find(ContainerClass))
			{
				for (const TSubclassOf<UObject>& DecorationClass : *RegisteredDecorations)
				{
					if (DecorationClass)
					{
						OutClasses.Add(DecorationClass);
					}
				}
			}
			ContainerClass = ContainerClass->GetSuperClass();
		}
	}

	virtual void GetAllRegisteredDecorationClasses(TSet<UClass*>& OutClasses) const override
	{
		for (const TPair<TSubclassOf<UMovieSceneDecorationContainerObject>, TSet<TSubclassOf<UObject>>>& Pair : CompatibleDecorations)
		{
			for (const TSubclassOf<UObject>& DecorationClass : Pair.Value)
			{
				if (DecorationClass)
				{
					OutClasses.Add(DecorationClass);
				}
			}
		}
	}

private:
	TSharedPtr<FMovieSceneModule> ModuleHandle;
	TMap<FName, FMovieSceneEvaluationGroupParameters> EvaluationGroupParameters;

	// Map from container class to set of compatible decoration classes
	TMap<TSubclassOf<UMovieSceneDecorationContainerObject>, TSet<TSubclassOf<UObject>>> CompatibleDecorations;

	TUniquePtr<UE::MovieScene::FPlaybackClientTracker> PlaybackClientTracker;
};

namespace UE::MovieScene
{

void TrackPlaybackClient(const TSharedRef<FSharedPlaybackState>& InSharedPlaybackState, IMovieScenePlaybackClient* InClient)
{
	if (FMovieSceneModule* Module = FModuleManager::GetModulePtr<FMovieSceneModule>("MovieScene"))
	{
		Module->TrackPlaybackClient(InSharedPlaybackState, InClient);
	}
}

} // namespace UE::MovieScene


IMPLEMENT_MODULE(FMovieSceneModule, MovieScene);
