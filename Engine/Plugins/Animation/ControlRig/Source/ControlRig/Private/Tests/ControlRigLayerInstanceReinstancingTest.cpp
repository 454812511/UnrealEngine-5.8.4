// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Sequencer/ControlRigLayerInstance.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace UE::ControlRig::Private
{
	// UAnimInstance declares ClassWithin as USkeletalMeshComponent, so every instance needs a
	// component outer.
	struct FLayerInstanceFixture
	{
		FLayerInstanceFixture()
			: SkeletalMeshComponent(NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient))
			, LayerInstance(NewObject<UControlRigLayerInstance>(SkeletalMeshComponent, NAME_None, RF_Transient))
			, OldSourceAnimInstance(NewObject<UAnimInstance>(SkeletalMeshComponent, NAME_None, RF_Transient))
			, NewSourceAnimInstance(NewObject<UAnimInstance>(SkeletalMeshComponent, NAME_None, RF_Transient))
		{
			SkeletalMeshComponent->AddToRoot();

			LayerInstance->SetSourceAnimInstance(OldSourceAnimInstance);
		}

		~FLayerInstanceFixture()
		{
			SkeletalMeshComponent->RemoveFromRoot();
		}

		USkeletalMeshComponent* SkeletalMeshComponent;
		UControlRigLayerInstance* LayerInstance;
		UAnimInstance* OldSourceAnimInstance;
		UAnimInstance* NewSourceAnimInstance;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlRigLayerInstanceRemapsReinstancedSource, "Animation.ControlRig.LayerInstance.RemapsReinstancedSource", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Recompiling the source Animation Blueprint replaces the instance the layer instance pulls its input
// pose from. The reinstancing hook has to follow that replacement, otherwise the layer instance keeps
// the old instance until it is collected and then has no input pose at all.
bool FControlRigLayerInstanceRemapsReinstancedSource::RunTest(const FString& Parameters)
{
	using namespace UE::ControlRig::Private;

	FLayerInstanceFixture Fixture;

	if (!TestEqual(TEXT("Source anim instance was set"), Fixture.LayerInstance->GetSourceAnimInstance(), Fixture.OldSourceAnimInstance))
	{
		return false;
	}

	TMap<UObject*, UObject*> OldToNewInstanceMap;
	OldToNewInstanceMap.Add(Fixture.OldSourceAnimInstance, Fixture.NewSourceAnimInstance);

	Fixture.LayerInstance->HandleObjectsReinstanced(OldToNewInstanceMap);

	TestEqual(TEXT("Source anim instance follows the reinstanced replacement"), Fixture.LayerInstance->GetSourceAnimInstance(), Fixture.NewSourceAnimInstance);

	// The reinstancer remaps the component's LinkedInstances array itself. This test drives the hook
	// directly, so the array still holds the original entry: what matters is that the hook left the
	// linked instance bookkeeping alone instead of unlinking on the way through.
	// Only the const accessor is public.
	const USkeletalMeshComponent* SkeletalMeshComponent = Fixture.SkeletalMeshComponent;
	TestTrue(TEXT("Hook left the component's linked instances alone"), SkeletalMeshComponent->GetLinkedAnimInstances().Contains(Fixture.OldSourceAnimInstance));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlRigLayerInstanceIgnoresUnrelatedReinstancing, "Animation.ControlRig.LayerInstance.IgnoresUnrelatedReinstancing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Every anim instance receives the reinstancing delegate, whatever was recompiled. A map that does not
// mention the source instance must leave the link alone.
bool FControlRigLayerInstanceIgnoresUnrelatedReinstancing::RunTest(const FString& Parameters)
{
	using namespace UE::ControlRig::Private;

	FLayerInstanceFixture Fixture;

	TMap<UObject*, UObject*> OldToNewInstanceMap;
	OldToNewInstanceMap.Add(Fixture.NewSourceAnimInstance, Fixture.OldSourceAnimInstance);

	Fixture.LayerInstance->HandleObjectsReinstanced(OldToNewInstanceMap);

	TestEqual(TEXT("Source anim instance unchanged"), Fixture.LayerInstance->GetSourceAnimInstance(), Fixture.OldSourceAnimInstance);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
