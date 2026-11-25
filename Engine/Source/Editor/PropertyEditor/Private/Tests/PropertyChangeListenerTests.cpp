// Copyright Epic Games, Inc. All Rights Reserved.

#include "PropertyChangeListenerTests.h"
#include "IPropertyChangeListener.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/StrongObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PropertyChangeListenerTests)

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A property change listener caches a value address per property, derived from the object that owns the value.  For a
 * nested object property that owner is the subobject, not the root object, and the listener regularly outlives it: a
 * Blueprint recompile reruns construction scripts, which destroys and recreates the actor's components while Sequencer
 * still holds a listener built from the old ones.  Scanning after that point must not touch the dead owner.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyEditorTests_PropertyChangeListenerOwnerDestroyed, "PropertyEditor.PropertyChangeListener.OwnerDestroyed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

#define REQUIRE(expr) \
do { \
	if (!(expr)) { \
		AddError(FString::Printf(TEXT("Expected expression '%s' to be true."), TEXT(#expr))); \
		return false; \
	} \
} while(false)

bool FPropertyEditorTests_PropertyChangeListenerOwnerDestroyed::RunTest(const FString& Parameters)
{
	const FName ValuePropertyName = GET_MEMBER_NAME_CHECKED(UPropertyChangeListenerTestSubObject, Value);
	const FName SubObjectPropertyName = GET_MEMBER_NAME_CHECKED(UPropertyChangeListenerTestObject, SubObject);

	TStrongObjectPtr<UPropertyChangeListenerTestObject> TestObject( NewObject<UPropertyChangeListenerTestObject>() );
	TestObject->SubObject = NewObject<UPropertyChangeListenerTestSubObject>( TestObject.Get() );
	TestObject->SubObject->Value = 1.0f;

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	TSharedPtr<IPropertyChangeListener> Listener = PropertyEditorModule.CreatePropertyChangeListener();
	REQUIRE(Listener.IsValid());

	TArray<FName> ReportedProperties;
	Listener->GetOnPropertyChangedDelegate().AddLambda(
		[&ReportedProperties](const TArray<UObject*>& ChangedObjects, const IPropertyHandle& Handle)
		{
			if (const FProperty* ChangedProperty = Handle.GetProperty())
			{
				ReportedProperties.Add( ChangedProperty->GetFName() );
			}
		});

	// Matches how Sequencer configures its listeners, which is what makes it walk into the subobject
	FPropertyListenerSettings Settings;
	Settings.bIgnoreArrayProperties = true;
	Settings.bIgnoreObjectProperties = false;
	Settings.RequiredPropertyFlags = 0;
	Settings.DisallowedPropertyFlags = CPF_EditConst;

	Listener->SetObject( *TestObject, Settings );

	// Confirm a cache really was built over the subobject before relying on that below
	TestObject->SubObject->Value = 2.0f;
	REQUIRE(Listener->ScanForChanges( /*bRecacheNewValues*/ true ));
	REQUIRE(ReportedProperties.Contains( ValuePropertyName ));

	// Retire the owner the subobject caches were built from, as rerunning construction scripts does to a component.
	// Its memory is deliberately still intact and mutated here: a listener that kept a raw pointer to the owner reads
	// the new value straight out of the retired object and reports a change for a property nobody can edit any more.
	UPropertyChangeListenerTestSubObject* RetiredSubObject = TestObject->SubObject;
	RetiredSubObject->MarkAsGarbage();
	RetiredSubObject->Value = 3.0f;

	ReportedProperties.Reset();
	Listener->ScanForChanges( /*bRecacheNewValues*/ false );
	REQUIRE(!ReportedProperties.Contains( ValuePropertyName ));

	// Now let the owner's memory actually go away.  This is the shape that crashed in UE-386906: the scan reached
	// FObjectPropertyNode::GetValueBaseAddress with a dangling owner and read the freed allocation as a UClass.
	ReportedProperties.Reset();
	TestObject->SubObject = nullptr;
	RetiredSubObject = nullptr;
	CollectGarbage( RF_NoFlags, /*bPerformFullPurge*/ true );

	Listener->ScanForChanges( /*bRecacheNewValues*/ false );
	REQUIRE(!ReportedProperties.Contains( ValuePropertyName ));

	// The reference going null is a real change on the surviving root object, so the scan is still doing its job here
	REQUIRE(ReportedProperties.Contains( SubObjectPropertyName ));

	// A second scan exercises the path where the previously cached value was dropped rather than recached
	ReportedProperties.Reset();
	Listener->ScanForChanges( /*bRecacheNewValues*/ true );
	REQUIRE(!ReportedProperties.Contains( ValuePropertyName ));

	return true;
}

#undef REQUIRE

#endif // WITH_DEV_AUTOMATION_TESTS
