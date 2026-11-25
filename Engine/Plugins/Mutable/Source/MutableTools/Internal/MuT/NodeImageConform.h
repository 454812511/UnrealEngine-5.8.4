// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/Image.h"
#include "MuR/Ptr.h"
#include "MuR/RefCounted.h"
#include "MuT/Node.h"
#include "MuT/NodeImage.h"

#define UE_API MUTABLETOOLS_API


namespace UE::Mutable::Private
{

	/** Node that conforms an image to a given image descriptor. */
	class NodeImageConform : public NodeImage
	{
	public:

		FImageDesc ConformDescriptor;
		Ptr<NodeImage> Source;

	public:

		// Node Interface
		virtual const FNodeType* GetType() const override { return &StaticType; }
		static const FNodeType* GetStaticType() { return &StaticType; }

	protected:

		/** Forbidden. Manage with the Ptr<> template. */
		~NodeImageConform() = default;

	private:

		static UE_API FNodeType StaticType;

	};


}

#undef UE_API
