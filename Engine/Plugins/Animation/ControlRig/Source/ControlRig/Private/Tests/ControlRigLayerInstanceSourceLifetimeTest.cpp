// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Sequencer/ControlRigLayerInstanceProxy.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/WeakObjectPtrTemplates.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace UE::ControlRig::Private
{
	// Counts how often the input pose node drives the source graph. The node reaches this through
	// FAnimInstanceProxy::UpdateInputProxy, which is the path taken when the source proxy has no
	// root node of its own.
	struct FCountingSourceProxy : public FAnimInstanceProxy
	{
		explicit FCountingSourceProxy(UAnimInstance* InAnimInstance)
			: FAnimInstanceProxy(InAnimInstance)
		{
		}

		virtual void UpdateAnimationNode(const FAnimationUpdateContext& InContext) override
		{
			NumUpdates++;
		}

		int32 NumUpdates = 0;
	};

	// Builds a live link from a stand-in layer instance to a stand-in source instance.
	//
	// The source proxy deliberately outlives the source instance. A real proxy is deleted by
	// UAnimInstance::BeginDestroy, so a regression would read freed memory and take the whole run
	// down. Keeping the proxy alive makes a regression report a failed expectation instead.
	struct FInputPoseFixture
	{
		FInputPoseFixture()
			// UAnimInstance declares ClassWithin as USkeletalMeshComponent, so both instances need a
			// component outer. An outer does not keep its subobjects alive, so the source instance is
			// still collectable while the component is rooted.
			: SkeletalMeshComponent(NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient))
			, LayerAnimInstance(NewObject<UAnimInstance>(SkeletalMeshComponent, NAME_None, RF_Transient))
			, SourceAnimInstance(NewObject<UAnimInstance>(SkeletalMeshComponent, NAME_None, RF_Transient))
			, LayerProxy(LayerAnimInstance)
			, SourceProxy(SourceAnimInstance)
			, WeakSourceAnimInstance(SourceAnimInstance)
			, UpdateContext(&LayerProxy, 1.0f / 30.0f)
		{
			// The layer instance stands in for the Control Rig layer instance, which stays alive
			// across a recompile of the source Animation Blueprint.
			SkeletalMeshComponent->AddToRoot();
			LayerAnimInstance->AddToRoot();

			InputPoseNode.Link(SourceAnimInstance, &SourceProxy);
		}

		~FInputPoseFixture()
		{
			LayerAnimInstance->RemoveFromRoot();
			SkeletalMeshComponent->RemoveFromRoot();
		}

		USkeletalMeshComponent* SkeletalMeshComponent;
		UAnimInstance* LayerAnimInstance;

		// Cleared to let the source instance become unreachable.
		UAnimInstance* SourceAnimInstance;

		FAnimInstanceProxy LayerProxy;
		FCountingSourceProxy SourceProxy;
		TWeakObjectPtr<UAnimInstance> WeakSourceAnimInstance;
		FAnimationUpdateContext UpdateContext;
		FAnimNode_ControlRigInputPose InputPoseNode;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlRigLayerInstanceSourceCollected, "Animation.ControlRig.LayerInstance.SourceAnimInstanceCollected", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Sequencer points a Control Rig layer instance at the component's own anim instance to supply its
// input pose. Nothing tells the input pose node when that source instance goes away, so the node has
// to notice by itself. Driving a collected source instance used to assert in
// FUObjectArray::IndexToObject, because the node still held a raw pointer to it.
bool FControlRigLayerInstanceSourceCollected::RunTest(const FString& Parameters)
{
	using namespace UE::ControlRig::Private;

	FInputPoseFixture Fixture;

	// The link is live, so the source graph runs.
	Fixture.InputPoseNode.Update_AnyThread(Fixture.UpdateContext);
	TestEqual(TEXT("Source graph updated while the source instance is alive"), Fixture.SourceProxy.NumUpdates, 1);

	Fixture.SourceAnimInstance = nullptr;
	CollectGarbage(RF_NoFlags, true);

	// Guard against a vacuous pass: the check below only means anything if the instance really went
	// away.
	if (!TestFalse(TEXT("Source anim instance was collected"), Fixture.WeakSourceAnimInstance.IsValid()))
	{
		return false;
	}

	// The node must now behave as if it were unlinked.
	Fixture.InputPoseNode.Update_AnyThread(Fixture.UpdateContext);
	TestEqual(TEXT("Source graph not updated after the source instance was collected"), Fixture.SourceProxy.NumUpdates, 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FControlRigLayerInstanceSourceReinstanced, "Animation.ControlRig.LayerInstance.SourceAnimInstanceReinstanced", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Recompiling the source Animation Blueprint reinstances the source anim instance, which marks the
// old one as garbage. The node has to drop the link at that point, not at the following collection:
// the layer instance can tick in between, and the source proxy is already on its way out.
bool FControlRigLayerInstanceSourceReinstanced::RunTest(const FString& Parameters)
{
	using namespace UE::ControlRig::Private;

	FInputPoseFixture Fixture;

	Fixture.InputPoseNode.Update_AnyThread(Fixture.UpdateContext);
	TestEqual(TEXT("Source graph updated while the source instance is alive"), Fixture.SourceProxy.NumUpdates, 1);

	// Reinstancing marks the replaced instance as garbage without collecting it yet.
	Fixture.SourceAnimInstance->MarkAsGarbage();

	Fixture.InputPoseNode.Update_AnyThread(Fixture.UpdateContext);
	TestEqual(TEXT("Source graph not updated after the source instance was marked as garbage"), Fixture.SourceProxy.NumUpdates, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
