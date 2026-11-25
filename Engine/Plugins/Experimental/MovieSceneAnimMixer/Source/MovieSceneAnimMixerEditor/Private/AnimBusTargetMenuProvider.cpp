// Copyright Epic Games, Inc. All Rights Reserved.

#include "AnimBusTargetMenuProvider.h"

#include "AnimMixerBusUtils.h"
#include "AnimMixerEditorBusUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ISequencer.h"
#include "MovieSceneAnimationMixerTrack.h"
#include "MovieSceneMixedAnimationTarget.h"
#include "MovieSceneSequence.h"
#include "Systems/MovieSceneAnimMixerSystem.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"

#define LOCTEXT_NAMESPACE "AnimBusTargetMenuProvider"

UScriptStruct* FAnimBusTargetMenuProvider::GetHandledTargetStructType() const
{
	return FMovieSceneAnimBusTarget::StaticStruct();
}

void FAnimBusTargetMenuProvider::PopulateTargetMenu(
	FMenuBuilder& MenuBuilder,
	TObjectPtr<UObject> BoundObject,
	TFunction<void(TInstancedStruct<FMovieSceneMixedAnimationTarget>)> OnTargetSelected,
	TSharedPtr<ISequencer> Sequencer,
	UMovieSceneAnimationMixerTrack* Track)
{
	MenuBuilder.AddSubMenu(
		LOCTEXT("BusTarget", "Bus Target"),
		LOCTEXT("BusTargetTooltip", "Write the mixer's evaluated pose to a named bus for consumption by other mixers."),
		FNewMenuDelegate::CreateRaw(this, &FAnimBusTargetMenuProvider::PopulateBusTargetSubmenu, BoundObject, OnTargetSelected, Sequencer, Track)
	);
}

void FAnimBusTargetMenuProvider::PopulateTargetMenu(
	FMenuBuilder& MenuBuilder,
	TObjectPtr<UObject> BoundObject,
	TFunction<void(TInstancedStruct<FMovieSceneMixedAnimationTarget>)> OnTargetSelected)
{
	PopulateTargetMenu(MenuBuilder, BoundObject, OnTargetSelected, nullptr, nullptr);
}

void FAnimBusTargetMenuProvider::PopulateBusTargetSubmenu(
	FMenuBuilder& MenuBuilder,
	TObjectPtr<UObject> BoundObject,
	TFunction<void(TInstancedStruct<FMovieSceneMixedAnimationTarget>)> OnTargetSelected,
	TSharedPtr<ISequencer> Sequencer,
	UMovieSceneAnimationMixerTrack* Track)
{
	// Buses are scoped per bound object, so list only those referenced by this
	// character's tracks. Resolve from the track, or from the bound object when
	// there is no track yet.
	TArray<UMovieSceneAnimationMixerTrack*> TracksForObject;
	if (Sequencer)
	{
		TracksForObject = Track
			? AnimMixerEditorBusUtils::GatherMixerTracksForSameObject(Track, *Sequencer)
			: AnimMixerEditorBusUtils::GatherMixerTracksForBoundObject(BoundObject, *Sequencer);
	}

	TArray<FName> KnownBuses = AnimMixerEditorBusUtils::GatherBusNamesFromTracks(TracksForObject);

	for (const FName& BusName : KnownBuses)
	{
		const bool bWouldCycle = Track &&
			FAnimMixerBusUtils::WouldBusTargetCreateCycle(BusName, Track, TracksForObject);

		MenuBuilder.AddMenuEntry(
			FText::FromName(BusName),
			bWouldCycle
				? FText::Format(LOCTEXT("BusTargetCycleTooltip", "Cannot target bus '{0}': would create a dependency cycle."), FText::FromName(BusName))
				: FText::Format(LOCTEXT("BusTargetEntryTooltip", "Write to bus '{0}'."), FText::FromName(BusName)),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([OnTargetSelected, BusName]()
				{
					OnTargetSelected(TInstancedStruct<FMovieSceneAnimBusTarget>::Make(BusName));
					FSlateApplication::Get().DismissAllMenus();
				}),
				FCanExecuteAction::CreateLambda([bWouldCycle]() { return !bWouldCycle; }))
		);
	}

	// "New Bus..." text entry
	TSharedRef<SEditableTextBox> TextBox = SNew(SEditableTextBox)
		.HintText(LOCTEXT("NewBusNameHint", "Enter bus name..."))
		.OnTextCommitted_Lambda([OnTargetSelected](const FText& InText, ETextCommit::Type CommitType)
		{
			if (CommitType == ETextCommit::OnEnter && !InText.IsEmpty())
			{
				FName BusName(*InText.ToString());
				OnTargetSelected(TInstancedStruct<FMovieSceneAnimBusTarget>::Make(BusName));
				FSlateApplication::Get().DismissAllMenus();
			}
		});

	// Focus the text box and claim the window's focus-on-activate widget, so a
	// co-hosted asset picker (in the add-track menu) can't reclaim keyboard focus
	// and divert typing to its search box.
	TextBox->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda(
		[WeakTextBox = TWeakPtr<SEditableTextBox>(TextBox)](double, float) -> EActiveTimerReturnType
		{
			TSharedPtr<SEditableTextBox> Box = WeakTextBox.Pin();
			if (!Box.IsValid())
			{
				return EActiveTimerReturnType::Stop;
			}

			FWidgetPath WidgetToFocusPath;
			if (FSlateApplication::Get().GeneratePathToWidgetUnchecked(Box.ToSharedRef(), WidgetToFocusPath) && WidgetToFocusPath.IsValid())
			{
				FSlateApplication::Get().SetKeyboardFocus(WidgetToFocusPath, EFocusCause::SetDirectly);
				if (TSharedPtr<SWindow> Window = WidgetToFocusPath.GetWindow())
				{
					Window->SetWidgetToFocusOnActivate(Box);
				}
				return EActiveTimerReturnType::Stop;
			}

			// Not in a window yet; retry next tick.
			return EActiveTimerReturnType::Continue;
		}));

	MenuBuilder.AddWidget(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.Padding(4.0f)
		.FillWidth(1.0f)
		[
			TextBox
		],
		LOCTEXT("NewBusLabel", "New Bus..."),
		/*bNoIndent=*/ true
	);
}

#undef LOCTEXT_NAMESPACE
