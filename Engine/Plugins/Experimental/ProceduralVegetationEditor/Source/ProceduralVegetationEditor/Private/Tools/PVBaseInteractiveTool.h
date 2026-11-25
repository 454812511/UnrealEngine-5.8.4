// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "AssetEditorMode/Tools/PCGAssetEditorInteractiveTool.h"

#include "GeometryCollection/ManagedArrayCollection.h"

#include "PVBaseInteractiveTool.generated.h"

struct FPCGTaggedData;
class IPCGBaseSubsystem;
class UPVData;
class UPVBaseSettings;
class UEditorInteractiveToolsContext;

UCLASS(Abstract)
class UPVBaseInteractiveTool : public UPCGAssetEditorInteractiveTool
{
	GENERATED_BODY()

public:
	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	
	virtual void RequestShutdown(EToolShutdownType ShutdownType);

	TObjectPtr<UWorld> GetTargetWorld();

	bool HasCollection() const;
	const TSharedPtr<FManagedArrayCollection>& GetCollection() const;

	template <typename T = UPVBaseSettings>
	TObjectPtr<T> GetNodeSettings() { return CastChecked<T>(NodeSettings); }

	template <typename T = UPVBaseSettings>
	TObjectPtr<const T> GetNodeSettings() const { return CastChecked<const T>(NodeSettings); }

protected:
	virtual void OnNodeExecuted() {}

private:
	void OnGraphExecuted(IPCGBaseSubsystem* InSubsystem, IPCGGraphExecutionSource* InExecutionSource, EPCGGenerationStatus InGenerationStatus);

	bool CacheData(const TArray<FPCGTaggedData>& InTaggedData);

protected:
	UPROPERTY(Transient)
	TObjectPtr<UWorld> TargetWorld = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<AActor> PreviewActor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UPVBaseSettings> NodeSettings = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UEditorInteractiveToolsContext> InteractiveToolsContext;

private:
	UPROPERTY(Transient)
	TObjectPtr<const UPVData> InputData = nullptr;
};
