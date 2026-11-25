// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetaHumanMassCrowdRepresentationActorManagement.h"

#include "Mass/MetaHumanMassRepresentationSubsystem.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "DefaultMovementSet/InstantMovementEffects/BasicInstantMovementEffects.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "MoverComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MetaHumanMassCrowdRepresentationActorManagement)

void UMetaHumanMassCrowdRepresentationActorManagement::SetActorEnabled(const EMassActorEnabledType EnabledType, AActor& Actor, const int32 EntityIdx, FMassCommandBuffer& CommandBuffer) const
{
	Super::SetActorEnabled(EnabledType, Actor, EntityIdx, CommandBuffer);

	// Visibility calculation is not always correct for MHs so we need to fix up.
	const bool bEnabled = EnabledType != EMassActorEnabledType::Disabled;

	TInlineComponentArray<UActorComponent*> ActorComponents;
	Actor.GetComponents(ActorComponents);

	static const FName HiddenTag("Hidden");
	
	for (UActorComponent* Component : ActorComponents)
	{
		Component->RegisterAllComponentTickFunctions(bEnabled && Component->PrimaryComponentTick.bStartWithTickEnabled);

		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			SceneComponent->SetVisibility(bEnabled && !SceneComponent->ComponentTags.Contains(HiddenTag));
		}
	}
}

void UMetaHumanMassCrowdRepresentationActorManagement::TeleportActor(const FTransform& Transform, AActor& Actor, FMassCommandBuffer& CommandBuffer) const
{
	// Base crowd teleport moves the actor's scene component (adds capsule half-height + ground-snap sweep) and commits it.
	Super::TeleportActor(Transform, Actor, CommandBuffer);

	// These MetaHuman crowd actors are driven by the Mover plugin, which keeps its OWN authoritative position
	// (FMoverDefaultSyncState). A plain component teleport does NOT update that sim state, so a pooled actor reused for a
	// new entity retains the previous occupant's position. In Walking mode the Mover integrates from that stale state and
	// snaps the actor back to it. Queue an FTeleportEffect: it teleports the actor AND writes the new transform
	// into the Mover sync state, applied in-sim (race-safe with the async Mover).
	if (UMoverComponent* MoverComp = Actor.FindComponentByClass<UMoverComponent>())
	{
		TSharedPtr<FTeleportEffect> TeleportEffect = MakeShared<FTeleportEffect>();
		TeleportEffect->TargetLocation = Actor.GetActorLocation();
		TeleportEffect->bUseActorRotation = true;
		MoverComp->QueueInstantMovementEffect(TeleportEffect);
	}
}
