// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

namespace UE::MetaHuman::Private
{
	// Set by FMetaHumanCharacterSkinMaterials::GetHeadPreviewMaterialInstance.
	inline bool bFacePreviewMaterialsFailed = false;

	// Set by FMetaHumanCharacterSkinMaterials::GetBodyPreviewMaterialInstance.
	inline bool bBodyPreviewMaterialsFailed = false;
}
