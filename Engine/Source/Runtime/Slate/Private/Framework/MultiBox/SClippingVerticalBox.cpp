// Copyright Epic Games, Inc. All Rights Reserved.

#include "Framework/MultiBox/SClippingVerticalBox.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/StyleColors.h"
#include "Layout/ArrangedChildren.h"
#include "Rendering/DrawElements.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/StyleDefaults.h"
#include "Styling/ToolBarStyle.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ClippingVerticalBox"

void SClippingVerticalBox::OnArrangeChildren( const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren ) const
{
	// If WrapButton hasn't been initialized, that means AddWrapButton() hasn't 
	// been called and this method isn't going to behave properly
	check(WrapButton.IsValid());

	ClippedWidgets.Empty();
	ClippedIndices.Empty();

	const FToolBarStyle& ToolBarStyle = StyleSet->GetWidgetStyle<FToolBarStyle>(StyleName);

	SVerticalBox::OnArrangeChildren(AllottedGeometry, ArrangedChildren);

	// Remove children that are clipped by the allotted geometry
	const int32 NumChildren = ArrangedChildren.Num();
	const int32 OverflowButtonIndex = NumChildren - 1;
	LastToolBarButtonIndex = OverflowButtonIndex - 1;

	const int32 OverflowButtonSize = ToolBarStyle.ButtonContentMaxWidth;

	for (int32 ChildIdx = LastToolBarButtonIndex; ChildIdx >= 0; --ChildIdx)
	{
		const FArrangedWidget& CurrentWidget = ArrangedChildren[ChildIdx];
		const FVector2D CurrentWidgetLocalPosition = CurrentWidget.Geometry.GetLocalPositionAtCoordinates(FVector2D::ZeroVector);

		/* if we're not on the last toolbar button, the button furthest Y should also take into account
		 the height of the wrap button so as not to be positioned over it */
		const int32 CurrentWidgetMaxY = LastToolBarButtonIndex != ChildIdx ?
			CurrentWidgetLocalPosition.Y + CurrentWidget.Geometry.Size.Y + OverflowButtonSize :
			CurrentWidgetLocalPosition.Y + CurrentWidget.Geometry.Size.Y; 
		
		if ( CurrentWidgetMaxY > AllottedGeometry.Size.Y)
		{
			// Insert the clipped widget at the start of the array to keep the array sorted with
			// the top-most clipped widget first and the bottom-most clipped widget last.
			ClippedWidgets.Insert(CurrentWidget.Widget.ToWeakPtr(), 0);
			ClippedIndices.Add(ChildIdx);

			// Drop the widget and remove it from the hit-test grid
			const_cast<SClippingVerticalBox*>(this)->InvalidateChildRemovedFromTree(CurrentWidget.Widget.Get());
			ArrangedChildren.Remove(ChildIdx);
		}
		else if (LastToolBarButtonIndex == ChildIdx)
		{
			// The bottom button fits, so nothing is clipped. The wrap button will be removed below
			break;
		}
	}

	// The wrap button is present only when something was clipped, so reposition it only then
	if (!ClippedIndices.IsEmpty())
	{
		// Something was clipped, so the wrap button is shown as the last arranged child
		const int32 ArrangedWrapButtonIndex = ArrangedChildren.Num() - 1;
		check(ArrangedChildren.Num() > 0 && ArrangedChildren[ArrangedWrapButtonIndex].Widget == Children[Children.Num() - 1].GetWidget());
		FArrangedWidget& ArrangedButton = ArrangedChildren[ArrangedWrapButtonIndex];
		FVector2D Size = ArrangedButton.Geometry.GetLocalSize();
		Size.Y = OverflowButtonSize;
		ArrangedButton.Geometry = AllottedGeometry.MakeChild(Size, FSlateLayoutTransform(AllottedGeometry.GetLocalSize() - Size));

		// ClippedIndices just changed, so re-evaluate the wrap button's bound BorderImage now to avoid a flicker
		WrapButtonBorder->UpdateAllAttributes();
	}
	else
	{
		// Nothing is clipped, so drop the wrap button and remove it from the hit-test grid
		const int32 ArrangedWrapButtonIndex = ArrangedChildren.Num() - 1;
		check(ArrangedChildren.Num() > 0 && ArrangedChildren[ArrangedWrapButtonIndex].Widget == Children[Children.Num() - 1].GetWidget());
		const_cast<SClippingVerticalBox*>(this)->InvalidateChildRemovedFromTree(ArrangedChildren[ArrangedWrapButtonIndex].Widget.Get());
		ArrangedChildren.Remove(ArrangedWrapButtonIndex);
	}

	RebuildFastPathChildren(ArrangedChildren);
}

FChildren* SClippingVerticalBox::GetAllChildren()
{
	return &FastPathChildren;
}

void SClippingVerticalBox::RebuildFastPathChildren(const FArrangedChildren& ArrangedChildren) const
{
	// GetAllChildren hands out FastPathChildren, so it has to mirror the children left in ArrangedChildren
	// after clipping. Raising ChildOrder on a change is what makes the fast path rebuild from the new view.
	bool bChanged = ArrangedChildren.Num() != FastPathChildren.Num();
	if (!bChanged)
	{
		for (int32 Index = 0; Index < ArrangedChildren.Num(); ++Index)
		{
			if (FastPathChildren[Index] != ArrangedChildren[Index].Widget)
			{
				bChanged = true;
				break;
			}
		}
	}

	if (!bChanged)
	{
		return;
	}

	FastPathChildren.Empty(ArrangedChildren.Num());
	for (int32 Index = 0; Index < ArrangedChildren.Num(); ++Index)
	{
		FastPathChildren.Add(ArrangedChildren[Index].Widget);
	}

	const_cast<SClippingVerticalBox*>(this)->Invalidate(EInvalidateWidgetReason::ChildOrder);
}

int32 SClippingVerticalBox::OnPaint( const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled ) const
{
	return SVerticalBox::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
}

FVector2D SClippingVerticalBox::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	FVector2D Size = SBoxPanel::ComputeDesiredSize(LayoutScaleMultiplier);
	{
		// If the wrap button isn't being shown, subtract it's size from the total desired size
		const SBoxPanel::FSlot& Child = Children[Children.Num() - 1];
		const FVector2D& ChildDesiredSize = Child.GetWidget()->GetDesiredSize();
		Size.Y -= ChildDesiredSize.Y;
	}
	return Size;
}

void SClippingVerticalBox::Construct( const FArguments& InArgs )
{
	OnWrapButtonClicked = InArgs._OnWrapButtonClicked;
	StyleSet = InArgs._StyleSet;
	StyleName = InArgs._StyleName;
	bIsFocusable = InArgs._IsFocusable;
	SelectedIndex = InArgs._SelectedIndex;
}

void SClippingVerticalBox::AddWrapButton()
{
	InitializeWrapButton();

	AddSlot()
	.Padding( 0.f )
	[
		WrapButton.ToSharedRef()
	];
}

void SClippingVerticalBox::InitializeWrapButton()
{
	const FToolBarStyle& ToolBarStyle = StyleSet->GetWidgetStyle<FToolBarStyle>(StyleName);
	const TSharedRef<SImage> IconWidget =
		SNew( SImage )
		.Visibility(EVisibility::HitTestInvisible)
		.Image( &ToolBarStyle.WrapButtonStyle.ExpandBrush );


	Style = ToolBarStyle.ButtonStyle;
	Style.SetNormalPadding(0 );
	Style.SetPressedPadding( 0 );

	// Highlight drawn behind the wrap button content while the selected item is in the overflow menu.
	SelectedBackgroundBrush = FSlateRoundedBoxBrush( FStyleColors::Primary, 4.f,
		FLinearColor(0, 0, 0, .8f), 0.5);

	// Construct the wrap button used in toolbars and menu bars
	// Always allow this to be focusable to prevent the menu from collapsing during interaction
	// clang-format off
	WrapButton =
		SNew( SComboButton )
		.HasDownArrow( false )
		.ButtonStyle( &Style )
		.ContentPadding( FMargin(0.f) )
		.VAlign( VAlign_Fill )
		.ToolTipText( NSLOCTEXT("Slate", "ExpandToolbar", "Click to expand toolbar") )
		.OnGetMenuContent_Lambda( [ this ] ()
		{
			TSharedRef<SWidget> Widget = OnWrapButtonClicked.Execute();
			return Widget;
		})
		.Cursor( EMouseCursor::Default )
		.OnMenuOpenChanged(this, &SClippingVerticalBox::OnWrapButtonOpenChanged)
		.IsFocusable(true)
		.ButtonContent()
		[
			SAssignNew(WrapButtonBorder, SBorder)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Center)
			.Padding( FMargin(2.f, 8.f) )
			.BorderImage_Lambda( [this] () -> const FSlateBrush*
			{
				return ClippedIndices.Contains(SelectedIndex.Get()) ? &SelectedBackgroundBrush : FStyleDefaults::GetNoBrush();
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.Padding(  ToolBarStyle.bShowLabels ? ToolBarStyle.IconPaddingWithVisibleLabel : ToolBarStyle.IconPadding )
				.AutoHeight()
				.HAlign(HAlign_Center)	// Center the icon horizontally, so that large labels don't stretch out the artwork
				[
					IconWidget
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Visibility( ToolBarStyle.bShowLabels ? EVisibility::Visible : EVisibility::Collapsed )
					.Text( LOCTEXT("ClippingVerticalBox.Icon.More", "More") )
					.TextStyle(&FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("SmallText"))
				]
			]
		];
	// clang-format on
}

void SClippingVerticalBox::OnWrapButtonOpenChanged(bool bIsOpen)
{
	if (bIsOpen && !WrapButtonOpenTimer.IsValid())
	{
		WrapButtonOpenTimer = RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SClippingVerticalBox::UpdateWrapButtonStatus));
	}
	else if(!bIsOpen && WrapButtonOpenTimer.IsValid())
	{
		UnRegisterActiveTimer(WrapButtonOpenTimer.ToSharedRef());
		WrapButtonOpenTimer.Reset();
	}
}

EActiveTimerReturnType SClippingVerticalBox::UpdateWrapButtonStatus(double CurrentTime, float DeltaTime)
{
	if (!WrapButton->IsOpen())
	{
		WrapButton->SetIsOpen(false);
		WrapButtonOpenTimer.Reset();
		return EActiveTimerReturnType::Stop;
	}

	return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE