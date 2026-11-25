// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassCrowdRepresentationActorManagement.h"
#include "MetaHumanMassCrowdRepresentationActorManagement.generated.h"

#define UE_API METAHUMANCROWD_API

UCLASS(MinimalAPI)
class UMetaHumanMassCrowdRepresentationActorManagement : public UMassCrowdRepresentationActorManagement
{
	GENERATED_BODY()

protected:

	// Override actor disabling / enabling as MetaHumans have a slightly different setup to normal UE Characters
	UE_API virtual void SetActorEnabled(const EMassActorEnabledType EnabledType, AActor& Actor, const int32 EntityIdx, FMassCommandBuffer& CommandBuffer) const override;

	// MetaHuman crowd actors are driven by the Mover plugin, which owns position via its own simulation state. Override to also teleport the Mover's sim state.
	UE_API virtual void TeleportActor(const FTransform& Transform, AActor& Actor, FMassCommandBuffer& CommandBuffer) const override;

};

#undef UE_API