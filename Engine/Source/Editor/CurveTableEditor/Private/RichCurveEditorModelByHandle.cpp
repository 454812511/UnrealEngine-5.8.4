// Copyright Epic Games, Inc. All Rights Reserved.

#include "RichCurveEditorModelByHandle.h"

#include "Curves/RichCurve.h"
#include "Misc/AssertionMacros.h"
#include "Templates/UnrealTemplate.h"

namespace UE::CurveTableEditor::AttributeModelDetail
{
static FRichCurve FallbackCurve;
}

FRichCurveEditorModelByHandle::FRichCurveEditorModelByHandle(FCurveTableEditorHandle InCurveHandle, UObject* InOwner)
	: FRichCurveEditorModel(InOwner)
	, CurveHandle(InCurveHandle)
{
	ensureMsgf(CurveHandle.IsValid(), TEXT("An unset curve handle leaves this model permanently invalid."));
}

bool FRichCurveEditorModelByHandle::IsValid() const
{
	return CurveHandle.IsValid();
}

FRichCurve& FRichCurveEditorModelByHandle::GetRichCurve()
{
	FRichCurve* Curve = CurveHandle.GetRichCurve();
	return ensure(Curve) ? *Curve : UE::CurveTableEditor::AttributeModelDetail::FallbackCurve;
}

const FRichCurve& FRichCurveEditorModelByHandle::GetReadOnlyRichCurve() const
{
	FRichCurve* Curve = CurveHandle.GetRichCurve();
	return ensure(Curve) ? *Curve : UE::CurveTableEditor::AttributeModelDetail::FallbackCurve;
}
