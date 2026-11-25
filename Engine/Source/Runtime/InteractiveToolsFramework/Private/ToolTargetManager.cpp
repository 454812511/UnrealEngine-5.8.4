// Copyright Epic Games, Inc. All Rights Reserved.


#include "ToolTargetManager.h"
#include "InteractiveToolsContext.h"
#include "ToolBuilderUtil.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ToolTargetManager)

namespace ToolTargetManagerLocals
{
	// Given an array and a 1:1 array of bools, remove all entries that have the corresponding bool set to true.
	// Returns number of elements removed
	int32 RemoveByFlags(TArray<UObject*>& ObjectsInOut, const TArray<bool>& RemoveFlags)
	{
		if (!ensure(RemoveFlags.Num() == ObjectsInOut.Num()))
		{
			return 0;
		}

		int32 OutIndex = 0;
		int32 InIndex = 0;
		while (InIndex < ObjectsInOut.Num())
		{
			if (!RemoveFlags[InIndex])
			{
				ObjectsInOut[OutIndex] = ObjectsInOut[InIndex];
				++OutIndex;
			}
			++InIndex;
		}
		ObjectsInOut.SetNum(OutIndex);
		return InIndex - OutIndex;
	}
}

void UToolTargetManager::Initialize()
{
	bIsActive = true;
}

void UToolTargetManager::Shutdown()
{
	Factories.Empty();
	bIsActive = false;
}

void UToolTargetManager::AddTargetFactory(UToolTargetFactory* Factory)
{
	// If this type of factory has already been added, skip it.
	if (Factories.ContainsByPredicate(
		[Factory](UToolTargetFactory* ExistingFactory) { 
			return ExistingFactory->GetClass() == Factory->GetClass(); 
		}))
	{
		return;
	}

	Factories.Add(Factory);
}

void UToolTargetManager::RemoveTargetFactoriesByPredicate(TFunctionRef<bool(UToolTargetFactory*)> Predicate)
{
	Factories.RemoveAll(Predicate);
}

UToolTargetFactory* UToolTargetManager::FindFirstFactoryByPredicate(TFunctionRef<bool(UToolTargetFactory*)> Predicate)
{
	TObjectPtr<UToolTargetFactory>* Found = Factories.FindByPredicate(Predicate);
	return (Found) ? Found->Get() : nullptr;
}


bool UToolTargetManager::CanBuildTarget(UObject* SourceObject, const FToolTargetTypeRequirements& TargetType) const
{
	if (InputFilterFunction && !InputFilterFunction(SourceObject))
	{
		return false;
	}
	for (UToolTargetFactory* Factory : Factories)
	{
		if (Factory->CanBuildTarget(SourceObject, TargetType))
		{
			return true;
		}
	}
	return false;
}

UToolTarget* UToolTargetManager::BuildTarget(UObject* SourceObject, const FToolTargetTypeRequirements& TargetType)
{
	if (InputFilterFunction && !InputFilterFunction(SourceObject))
	{
		return nullptr;
	}
	for (UToolTargetFactory* Factory : Factories)
	{
		if (Factory->CanBuildTarget(SourceObject, TargetType))
		{
			UToolTarget* Result = Factory->BuildTarget(SourceObject, TargetType);
			if (Result != nullptr)
			{
				return Result;
			}
		}
	}
	return nullptr;
}

int32 UToolTargetManager::CountSelectedAndTargetable(const FToolBuilderState& SceneState, const FToolTargetTypeRequirements& TargetType) const
{
	return CountSelectedAndTargetableWithPredicate(SceneState, TargetType, 
		// We currently only ever get components out of our scene state, so it is ok to
		//  use the predicate that takes an actor component
		[](UActorComponent& Component) { return true; });
}


void UToolTargetManager::EnumerateSelectedAndTargetableComponents(const FToolBuilderState& SceneState,
	const FToolTargetTypeRequirements& TargetRequirements,
	TFunctionRef<void(UActorComponent*)> ComponentFunc) const
{
	// Gather input objects
	TArray<UObject*> InputObjects;
	InputObjects.Append(ToolBuilderUtil::FindAllComponents(SceneState, [this](UActorComponent* Component)
	{
		return Component && (!InputFilterFunction || InputFilterFunction(Component));
	}));

	// Targets are built factory-by-factory (in factory registration order), but some tools
	// distinguish inputs by selection order. Flag which inputs get claimed, then fire ComponentFunc
	// over InputObjects in selection order rather than factory-registration order. A set flag also
	// marks an input as no longer present in Remaining, which lets us map each factory's Used flags
	// (1:1 with the shrinking Remaining set) back onto InputObjects.
	TArray<bool> Targetable;
	Targetable.Init(false, InputObjects.Num());

	TArray<UObject*> Remaining = InputObjects;
	TArray<bool> Used;
	for (TObjectPtr<UToolTargetFactory> Factory : Factories)
	{
		if (Remaining.IsEmpty())
		{
			break;
		}
		if (Factory->CanBuildTargets(Remaining, TargetRequirements, Used) > 0)
		{
			int32 RemainingIndex = 0;
			for (int32 Index = 0; Index < InputObjects.Num(); ++Index)
			{
				if (!Targetable[Index])		// still present in Remaining
				{
					Targetable[Index] = Used[RemainingIndex++];
				}
			}
			ensureMsgf(ToolTargetManagerLocals::RemoveByFlags(Remaining, Used) > 0,
				TEXT("Factory claimed it could build target(s) without using any objects."));
		}
	}

	for (int32 Index = 0; Index < InputObjects.Num(); ++Index)
	{
		if (Targetable[Index])
		{
			ComponentFunc(static_cast<UActorComponent*>(InputObjects[Index]));
		}
	}
}

int32 UToolTargetManager::CountSelectedAndTargetableWithPredicate(const FToolBuilderState& SceneState,
	const FToolTargetTypeRequirements& TargetRequirements,
	TFunctionRef<bool(UActorComponent&)> ComponentPred) const
{
	// Gather input objects
	TArray<UObject*> InputObjects;
	InputObjects.Append(ToolBuilderUtil::FindAllComponents(SceneState, [this, &ComponentPred](UActorComponent* Component)
	{
		return Component 
			&& (!InputFilterFunction || InputFilterFunction(Component)) 
			&& ComponentPred(*Component);
	}));

	int32 Count = 0;
	TArray<bool> Used;
	for (TObjectPtr<UToolTargetFactory> Factory : Factories)
	{
		int32 NumTargets = Factory->CanBuildTargets(InputObjects, TargetRequirements, Used);
		if (NumTargets > 0)
		{
			Count += NumTargets;
			ensureMsgf(ToolTargetManagerLocals::RemoveByFlags(InputObjects, Used) > 0,
				TEXT("Factory claimed it could build target(s) without using any objects."));
			if (InputObjects.IsEmpty())
			{
				break;
			}
		}
	}

	return Count;
}

UToolTarget* UToolTargetManager::BuildFirstSelectedTargetable(const FToolBuilderState& SceneState, const FToolTargetTypeRequirements& TargetType)
{
	// Gather input objects
	TArray<UObject*> InputObjects;
	InputObjects.Append(ToolBuilderUtil::FindAllComponents(SceneState, [this](UActorComponent* Component)
	{
		return Component && (!InputFilterFunction || InputFilterFunction(Component));
	}));

	// InputObjects is in selection order and is not mutated here, so an input's index is its
	// selection index. Pick the factory that would claim the earliest-selected input so that
	// "first targetable" means first in selection order, not first in factory registration order.
	// Ties (an input claimable by multiple factories) resolve to the earliest-registered factory.
	UToolTargetFactory* FirstFactory = nullptr;
	int32 FirstIndex = MAX_int32;
	TArray<bool> Used;
	for (TObjectPtr<UToolTargetFactory> Factory : Factories)
	{
		if (Factory->CanBuildTargets(InputObjects, TargetType, Used) > 0)
		{
			for (int32 Index = 0; Index < Used.Num(); ++Index)
			{
				if (Used[Index])
				{
					if (Index < FirstIndex)
					{
						FirstIndex = Index;
						FirstFactory = Factory;
					}
					break;
				}
			}
		}
	}

	return FirstFactory ? FirstFactory->BuildFirstTarget(InputObjects, TargetType, Used) : nullptr;
}

TArray<TObjectPtr<UToolTarget>> UToolTargetManager::BuildAllSelectedTargetable(const FToolBuilderState& SceneState,
	const FToolTargetTypeRequirements& TargetType)
{
	// Gather input objects
	TArray<UObject*> InputObjects;
	InputObjects.Append(ToolBuilderUtil::FindAllComponents(SceneState, [this](UActorComponent* Component)
	{
		return Component && (!InputFilterFunction || InputFilterFunction(Component));
	}));

	// Targets are built factory-by-factory (in factory registration order), but some tools
	// distinguish targets by selection order. Collect each input's target(s) into a per-input slot,
	// then emit the slots in selection order. A claimed flag also marks an input as no longer
	// present in Remaining, which lets us map each factory's Used flags (1:1 with the shrinking
	// Remaining set) back onto InputObjects.
	TArray<TArray<UToolTarget*>> TargetsPerInput;
	TargetsPerInput.SetNum(InputObjects.Num());
	TArray<bool> Claimed;
	Claimed.Init(false, InputObjects.Num());

	TArray<UObject*> Remaining = InputObjects;
	TArray<bool> Used;
	for (TObjectPtr<UToolTargetFactory> Factory : Factories)
	{
		if (Remaining.IsEmpty())
		{
			break;
		}
		if (Factory->CanBuildTargets(Remaining, TargetType, Used) > 0)
		{
			TArray<UToolTarget*> Built = Factory->BuildTargets(Remaining, TargetType, Used);

			// Map Used (1:1 with Remaining) onto the InputObjects positions claimed this round.
			// These are ascending, so ClaimedThisRound[0] is the earliest-selected claimed input.
			TArray<int32> ClaimedThisRound;
			int32 RemainingIndex = 0;
			for (int32 Index = 0; Index < InputObjects.Num(); ++Index)
			{
				if (!Claimed[Index])		// still present in Remaining
				{
					if (Used[RemainingIndex++])
					{
						Claimed[Index] = true;
						ClaimedThisRound.Add(Index);
					}
				}
			}

			if (Built.Num() == ClaimedThisRound.Num())
			{
				// Common 1:1 case: place each target at the input it was built from.
				for (int32 Index = 0; Index < Built.Num(); ++Index)
				{
					TargetsPerInput[ClaimedThisRound[Index]].Add(Built[Index]);
				}
			}
			else if (ClaimedThisRound.Num() > 0)
			{
				// Combined targets: the factory merged several inputs into fewer targets and does
				// not expose per-target attribution, so place them at the earliest input claimed.
				TargetsPerInput[ClaimedThisRound[0]].Append(Built);
			}

			ensureMsgf(ToolTargetManagerLocals::RemoveByFlags(Remaining, Used) > 0,
				TEXT("Factory claimed it could build target(s) without using any objects."));
		}
	}

	TArray<TObjectPtr<UToolTarget>> TargetsOut;
	for (const TArray<UToolTarget*>& InputTargets : TargetsPerInput)
	{
		for (UToolTarget* Target : InputTargets)
		{
			TargetsOut.Add(Target);
		}
	}
	return TargetsOut;
}

