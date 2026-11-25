// Copyright Epic Games, Inc. All Rights Reserved.

#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "EntitySystem/MovieSceneEntitySystemLinker.h"
#include "EntitySystem/MovieSceneEntitySystemRunner.h"
#include "Evaluation/MovieSceneEvaluationTemplateInstance.h"
#include "IMovieScenePlayer.h"
#include "Misc/AutomationTest.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tests/MovieScenePartialEvaluationTests.h"
#include "Tests/MovieSceneTestDataBuilders.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Variants/MovieSceneTimeWarpCurve.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{

constexpr int32 FirstSectionStart  = 0;
constexpr int32 FirstSectionEnd    = 1000;
constexpr int32 SecondSectionStart = 2000;
constexpr int32 SecondSectionEnd   = 3000;

constexpr float FirstSectionValue  = 100.f;
constexpr float SecondSectionValue = 500.f;

/** Root time at which the stepped time-warp jumps from the first section to the second. */
constexpr int32 StepTime = 1000;

/** Sub times the warp holds either side of the step. */
constexpr int32 TimeWithinFirstSection  = 500;
constexpr int32 TimeWithinSecondSection = 2500;

// AUTORTFM_ENABLE FTimeWarpTestPlayer because IMovieScenePlayer is AutoRTFM-enabled.
struct AUTORTFM_ENABLE FTimeWarpTestPlayer : IMovieScenePlayer
{
	FMovieSceneRootEvaluationTemplateInstance Template;
	UMovieSceneEntitySystemLinker* TestLinker = nullptr;

	virtual FMovieSceneRootEvaluationTemplateInstance& GetEvaluationTemplate() override { return Template; }
	virtual UMovieSceneEntitySystemLinker* ConstructEntitySystemLinker() override { return TestLinker; }
	virtual EMovieScenePlayerStatus::Type GetPlaybackStatus() const override { return EMovieScenePlayerStatus::Playing; }
	virtual void SetPlaybackStatus(EMovieScenePlayerStatus::Type InPlaybackStatus) override {}
};

/**
 * A root sequence containing a single sub section, evaluated through a real linker and runner.
 *
 * The sub sequence animates FloatProperty from two non-overlapping sections, so the value read back after an
 * evaluation tells you which of the two sections currently has entities imported.
 */
struct FTimeWarpEvaluationHarness
{
	TStrongObjectPtr<UMovieScenePartialEvaluationTestObject> TestObject;
	TStrongObjectPtr<UMovieSceneSequence> SubSequence;
	TStrongObjectPtr<UMovieSceneSequence> RootSequence;
	TStrongObjectPtr<UMovieSceneEntitySystemLinker> Linker;
	TSharedPtr<FMovieSceneEntitySystemRunner> Runner;
	FTimeWarpTestPlayer Player;
	UMovieSceneSubSection* SubSection = nullptr;

	FTimeWarpEvaluationHarness()
	{
		using namespace UE::MovieScene::Test;

		TestObject.Reset(NewObject<UMovieScenePartialEvaluationTestObject>());

		FSequenceBuilder SubSequenceBuilder;
		SubSequenceBuilder
			.AddObjectBinding(TestObject.Get())
			.AddPropertyTrack<UMovieSceneFloatTrack>(GET_MEMBER_NAME_CHECKED(UMovieScenePartialEvaluationTestObject, FloatProperty))
				.AddSection(FirstSectionStart, FirstSectionEnd)
					.AddKey<FMovieSceneFloatChannel, float>(0, FirstSectionStart, FirstSectionValue)
				.Pop()
				.AddSection(SecondSectionStart, SecondSectionEnd)
					.AddKey<FMovieSceneFloatChannel, float>(0, SecondSectionStart, SecondSectionValue)
				.Pop()
			.Pop();

		SubSequence.Reset(SubSequenceBuilder.Sequence);
		SubSequence->GetMovieScene()->SetPlaybackRange(TRange<FFrameNumber>(FirstSectionStart, SecondSectionEnd));

		UMovieSceneSequence* SubSequencePtr = SubSequence.Get();

		FSequenceBuilder RootSequenceBuilder;
		RootSequenceBuilder
			.AddRootTrack<UMovieSceneSubTrack>()
				.AddSection(FirstSectionStart, SecondSectionEnd)
					.Do<UMovieSceneSubSection>([SubSequencePtr](UMovieSceneSubSection* InSubSection){ InSubSection->SetSequence(SubSequencePtr); })
					.Assign(SubSection)
				.Pop()
			.Pop();

		RootSequence.Reset(RootSequenceBuilder.Sequence);
		RootSequence->GetMovieScene()->SetPlaybackRange(TRange<FFrameNumber>(FirstSectionStart, SecondSectionEnd));
	}

	/** Replaces the sub section's time scale with a curve that steps from the first section to the second at StepTime. */
	void ApplySteppedTimeWarp()
	{
		UMovieSceneTimeWarpCurve* Curve = NewObject<UMovieSceneTimeWarpCurve>(SubSection);

		FMovieSceneDoubleValue FirstKey(TimeWithinFirstSection);
		FirstKey.InterpMode = RCIM_Constant;

		FMovieSceneDoubleValue SecondKey(TimeWithinSecondSection);
		SecondKey.InterpMode = RCIM_Constant;

		TMovieSceneChannelData<FMovieSceneDoubleValue> ChannelData = Curve->Channel.GetData();
		ChannelData.AddKey(FFrameNumber(FirstSectionStart), FirstKey);
		ChannelData.AddKey(FFrameNumber(StepTime), SecondKey);

		SubSection->Parameters.TimeScale.Set(Curve);
	}

	/** Compiles the root sequence and prepares the linker. Must be called after any time warp has been applied. */
	void Initialize()
	{
		UMovieSceneCompiledDataManager* CompiledDataManager = UMovieSceneCompiledDataManager::GetPrecompiledData();

		Linker.Reset(NewObject<UMovieSceneEntitySystemLinker>(GetTransientPackage()));
		Runner = Linker->GetRunner();

		Player.TestLinker = Linker.Get();

		CompiledDataManager->Compile(RootSequence.Get());
		Player.Template.Initialize(*RootSequence, Player, CompiledDataManager);
	}

	/** Evaluates the root sequence at the given root time and returns the resulting animated property value. */
	float EvaluateAt(int32 RootTime)
	{
		const FFrameRate TickResolution = RootSequence->GetMovieScene()->GetTickResolution();

		Runner->QueueUpdate(
			FMovieSceneContext(FMovieSceneEvaluationRange(FFrameTime(RootTime), TickResolution), EMovieScenePlayerStatus::Playing),
			Player.Template.GetRootInstanceHandle());
		Runner->Flush();

		return TestObject->FloatProperty;
	}

	void Finish()
	{
		Player.Template.TearDown();
		Runner->Flush();
	}
};

} // anonymous namespace

/**
 * A stepped time warp jumps the sub sequence from one section to another in a single frame. Checks that the section
 * on the far side of the jump is the one animating afterwards.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMovieSceneSteppedTimeWarpEntityRefreshTest,
		"System.Engine.Sequencer.TimeWarp.SteppedTimeWarpEntityRefresh",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMovieSceneSteppedTimeWarpEntityRefreshTest::RunTest(const FString& Parameters)
{
	FTimeWarpEvaluationHarness Harness;
	Harness.ApplySteppedTimeWarp();
	Harness.Initialize();

	const float BeforeStep = Harness.EvaluateAt(StepTime - 1);
	UTEST_EQUAL("Before the step, the first section should be animating", BeforeStep, FirstSectionValue);

	const float AfterStep = Harness.EvaluateAt(StepTime);
	UTEST_EQUAL("After the step, the second section should be animating", AfterStep, SecondSectionValue);

	Harness.Finish();
	return true;
}

/**
 * The same section boundary crossed without a time warp, so the untimewarped path stays covered.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMovieSceneLinearSubSectionEntityRefreshTest,
		"System.Engine.Sequencer.TimeWarp.LinearSubSectionEntityRefresh",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMovieSceneLinearSubSectionEntityRefreshTest::RunTest(const FString& Parameters)
{
	FTimeWarpEvaluationHarness Harness;
	Harness.Initialize();

	const float WithinFirst = Harness.EvaluateAt(TimeWithinFirstSection);
	UTEST_EQUAL("The first section should be animating", WithinFirst, FirstSectionValue);

	const float WithinSecond = Harness.EvaluateAt(TimeWithinSecondSection);
	UTEST_EQUAL("The second section should be animating", WithinSecond, SecondSectionValue);

	Harness.Finish();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
