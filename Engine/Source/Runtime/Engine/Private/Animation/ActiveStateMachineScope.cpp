// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/ActiveStateMachineScope.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Async/Mutex.h"
#include "Containers/ArrayView.h"
#include "Containers/Set.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ActiveStateMachineScope)

FEncounteredStateMachineStack::FEncounteredStateMachineStack(const FEncounteredStateMachineStack& ParentStack, int32 InStateMachineIndex, int32 InStateIndex) :
	StateStack(ParentStack.StateStack)
{
	StateStack.Emplace(InStateMachineIndex, InStateIndex);
}

FEncounteredStateMachineStack::FEncounteredStateMachineStack(int32 InStateMachineIndex, int32 InStateIndex)
{
	StateStack.Emplace(InStateMachineIndex, InStateIndex);
}

FEncounteredStateMachineStack FEncounteredStateMachineStack::InitStack(const FEncounteredStateMachineStack& ParentStack, int32 InStateMachineIndex, int32 InStateIndex)
{
	return FEncounteredStateMachineStack(ParentStack, InStateMachineIndex, InStateIndex); 
}

FEncounteredStateMachineStack FEncounteredStateMachineStack::InitStack(int32 InStateMachineIndex, int32 InStateIndex)
{
	return FEncounteredStateMachineStack(InStateMachineIndex, InStateIndex);
}

namespace
{
	static bool bClampStateMachineContextStack = true;
	FAutoConsoleVariableRef CVarClampStateMachineContextStack(
		TEXT("a.ClampStateMachineContextStack"),
		bClampStateMachineContextStack,
		TEXT("When enabled EncounteredStateMachineStack::StateStack will be clamped to a max of 4 to avoid an unsafe reallocation with MemStackAllocator. Entries past the 4th element will use separate storage. Only disable if source changes are made to remove the secondary allocator from StateStack. \n")
		TEXT("0: Disable, 1: Enable"),
		ECVF_ReadOnly);

	// Identifies which live notify contexts are full-depth contexts so GetEffectiveStateStack can
	// downcast safely. Removal runs in the virtual destructor, so an entry cannot outlive its context.
	struct FFullDepthContextRegistry
	{
		UE::FMutex Mutex;
		TSet<const UE::Anim::FAnimNotifyStateMachineContext*> LiveContexts;

		static FFullDepthContextRegistry& Get()
		{
			// Leaked so contexts destroyed during static shutdown can still unregister safely.
			static FFullDepthContextRegistry* Registry = new FFullDepthContextRegistry();
			return *Registry;
		}
	};

	// Holds the full state stack when nesting exceeds the inline capacity of the public stack, which
	// cannot grow safely (see ClampStackToInlineCapacity). GetTypeName is final in the base, so this
	// stays discoverable as FAnimNotifyStateMachineContext through GetContextData.
	class FAnimNotifyStateMachineFullDepthContext final : public UE::Anim::FAnimNotifyStateMachineContext
	{
	public:
		explicit FAnimNotifyStateMachineFullDepthContext(const FEncounteredStateMachineStack& InEncounteredStateMachines)
			: UE::Anim::FAnimNotifyStateMachineContext(InEncounteredStateMachines)
		{
			FullStateStack.Append(InEncounteredStateMachines.StateStack.GetData(), InEncounteredStateMachines.StateStack.Num());

			FFullDepthContextRegistry& Registry = FFullDepthContextRegistry::Get();
			UE::TScopeLock RegistryLock(Registry.Mutex);
			Registry.LiveContexts.Add(this);
		}

		virtual ~FAnimNotifyStateMachineFullDepthContext() override
		{
			FFullDepthContextRegistry& Registry = FFullDepthContextRegistry::Get();
			UE::TScopeLock RegistryLock(Registry.Mutex);
			Registry.LiveContexts.Remove(this);
		}

		TArray<FEncounteredStateMachineStack::FStateMachineEntry> FullStateStack;
	};
}

namespace UE { namespace Anim {

IMPLEMENT_NOTIFY_CONTEXT_INTERFACE(FAnimNotifyStateMachineContext)

// The notify context's copy must stay within StateStack's inline buffer: spilled entries come from
// the update's FMemStack, which is freed once the FMemMark in FAnimInstanceProxy::UpdateAnimation
// pops, long before the context is read. Deeper entries are dropped here; the engine path recovers
// them from FAnimNotifyStateMachineFullDepthContext.
static FEncounteredStateMachineStack ClampStackToInlineCapacity(const FEncounteredStateMachineStack& SourceStack)
{
	if (bClampStateMachineContextStack)
	{
		FEncounteredStateMachineStack ClampedStack;
		const int32 NumEntriesToKeep = FMath::Min(SourceStack.StateStack.Num(), ClampedStack.StateStack.Max());
		for (int32 EntryIndex = 0; EntryIndex < NumEntriesToKeep; ++EntryIndex)
		{
			ClampedStack.StateStack.Add(SourceStack.StateStack[EntryIndex]);
		}
		return ClampedStack;
	}

	// Pre hotfix behaviour, for modified source builds where StateStack storage is safely reallocated
	return SourceStack;
}

namespace Private
{

TConstArrayView<FEncounteredStateMachineStack::FStateMachineEntry> GetEffectiveStateStack(const FAnimNotifyStateMachineContext& Context)
{
	// Only clamped stacks can have a full-depth companion, and clamping always fills the inline buffer.
	if (bClampStateMachineContextStack && Context.EncounteredStateMachines.StateStack.Num() == FEncounteredStateMachineStack().StateStack.Max())
	{
		FFullDepthContextRegistry& Registry = FFullDepthContextRegistry::Get();
		UE::TScopeLock RegistryLock(Registry.Mutex);
		if (Registry.LiveContexts.Contains(&Context))
		{
			return static_cast<const FAnimNotifyStateMachineFullDepthContext&>(Context).FullStateStack;
		}
	}
	return Context.EncounteredStateMachines.StateStack;
}

}

FAnimNotifyStateMachineContext::FAnimNotifyStateMachineContext(const FEncounteredStateMachineStack& InEncounteredStateMachines)
	: EncounteredStateMachines(ClampStackToInlineCapacity(InEncounteredStateMachines))
{
}

bool FAnimNotifyStateMachineContext::IsStateMachineInContext(int32 StateMachineIndex) const
{
	for (const FEncounteredStateMachineStack::FStateMachineEntry& Entry : Private::GetEffectiveStateStack(*this))
	{
		if (Entry.StateMachineIndex == StateMachineIndex)
		{
			return true;
		}
	}
	return false;
}

bool FAnimNotifyStateMachineContext::IsStateInStateMachineInContext(int32 StateMachineIndex, int32 StateIndex) const
{
	for (const FEncounteredStateMachineStack::FStateMachineEntry& Entry : Private::GetEffectiveStateStack(*this))
	{
		if (Entry.StateMachineIndex == StateMachineIndex && Entry.StateIndex == StateIndex)
		{
			return true;
		}
	}
	return false;
}

IMPLEMENT_ANIMGRAPH_MESSAGE(FActiveStateMachineScope);

FActiveStateMachineScope::FActiveStateMachineScope(const FAnimationBaseContext& InContext, FAnimNode_StateMachine* StateMachine, int32 InStateIndex)
	: ActiveStateMachines([&InContext, StateMachine, InStateIndex]()
	{
		const int32 StateMachineIndex = GetStateMachineIndex(StateMachine, InContext);
		if (FActiveStateMachineScope* ParentStateMachineScope = InContext.GetMessage<FActiveStateMachineScope>())
		{
			return FEncounteredStateMachineStack::InitStack(ParentStateMachineScope->ActiveStateMachines, StateMachineIndex, InStateIndex);
		}
		else
		{
			return FEncounteredStateMachineStack::InitStack(StateMachineIndex, InStateIndex);
		}
	}())
{
}

int32 FActiveStateMachineScope::GetStateMachineIndex(FAnimNode_StateMachine* StateMachine, const FAnimationBaseContext& Context)
{
	if (Context.AnimInstanceProxy && Context.AnimInstanceProxy->GetAnimClassInterface())
	{
		const int32 NumProperties = Context.AnimInstanceProxy->GetAnimClassInterface()->GetAnimNodeProperties().Num();
		const int32 StateMachineIndex = NumProperties - 1 - StateMachine->GetNodeIndex();
		return StateMachineIndex;
	}
	return INDEX_NONE;
}

TUniquePtr<const IAnimNotifyEventContextDataInterface> FActiveStateMachineScope::MakeUniqueEventContextData() const
{
	// Nesting deeper than the inline capacity cannot be published through the public stack (spilled
	// entries would live on the update's FMemStack and dangle once its FMemMark pops), so use the
	// private context that keeps the full stack on the heap alongside a clamped public copy.
	if (bClampStateMachineContextStack 
		&& ActiveStateMachines.StateStack.Num() > FEncounteredStateMachineStack().StateStack.Max())
	{
		return MakeUnique<const FAnimNotifyStateMachineFullDepthContext>(ActiveStateMachines);
	}

	return MakeUnique<const FAnimNotifyStateMachineContext>(ActiveStateMachines);
}
	
}}	// namespace UE::Anim
