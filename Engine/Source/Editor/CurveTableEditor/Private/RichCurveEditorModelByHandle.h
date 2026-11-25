// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CurveTableEditorHandle.h"
#include "RichCurveEditorModel.h"

class UObject;
struct FRichCurve;

/**
 * Rich curve model that resolves its curve through an attribute on every access instead of caching a raw pointer.
 *
 * UCurveTable stores its rows as individual heap allocations which are deleted and reallocated wholesale whenever the
 * table is re-serialized, as happens during an undo. A model holding a raw FRichCurve* is left pointing at freed memory
 * until the owning editor rebuilds it, so anything reading the model in the meantime - selection cleansing, key
 * proxies, drawing - reads that freed memory.
 */
class FRichCurveEditorModelByHandle : public FRichCurveEditorModel
{
public:

	FRichCurveEditorModelByHandle(FCurveTableEditorHandle InCurveHandle, UObject* InOwner);

	//~ Begin FRichCurveEditorModel Interface
	virtual bool IsValid() const override;
	virtual FRichCurve& GetRichCurve() override;
	virtual const FRichCurve& GetReadOnlyRichCurve() const override;
	//~ End FRichCurveEditorModel Interface

private:

	/** Re-resolved on every access. */
	const FCurveTableEditorHandle CurveHandle;
};
