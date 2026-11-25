// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "UObject/Object.h"

#include "PropertyChangeListenerTests.generated.h"

#if WITH_EDITORONLY_DATA

/** Stands in for a component: an instanced subobject the listener will cache property values from */
UCLASS(EditInlineNew, DefaultToInstanced)
class UPropertyChangeListenerTestSubObject : public UObject
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, Category = "Properties")
	float Value = 0.0f;
};

/** Stands in for an actor owning that component */
UCLASS()
class UPropertyChangeListenerTestObject : public UObject
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, Instanced, Category = "Properties")
	TObjectPtr<UPropertyChangeListenerTestSubObject> SubObject;
};

#endif // WITH_EDITORONLY_DATA
