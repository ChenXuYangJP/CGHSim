#include "CGH/Actors/CGHSolverActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverDuplicateTest,
	"CGH.SolverActor.DuplicateDoesNotInheritQueuedJob",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverDuplicateTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Game))
	{
		Scene.ForwardErrorMessages(this);
		return false;
	}
	UWorld* World = Scene.GetTestWorld();
	ACGHSLMActor* SLM = World->SpawnActor<ACGHSLMActor>();
	ACGHReconstructionLightActor* Light = World->SpawnActor<ACGHReconstructionLightActor>();
	ACGHTargetActor* Target = World->SpawnActor<ACGHTargetActor>();
	ACGHWorkbenchActor* Workbench = World->SpawnActor<ACGHWorkbenchActor>();
	ACGHSolverActor* Source = World->SpawnActor<ACGHSolverActor>();
	if (!SLM || !Light || !Target || !Workbench || !Source)
	{
		AddError(TEXT("Could not spawn isolated solver duplication fixture."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		Source->CancelSolve();
	};
	SLM->Parameters.ResolutionX = 4;
	SLM->Parameters.ResolutionY = 3;
	SLM->ClearPhasePattern();
	Target->SetActorLocation(FVector(50.0, 1.0, -2.0));
	Workbench->SLM = SLM;
	Workbench->ReconstructionLight = Light;
	Workbench->Targets = {Target};
	Workbench->Solver = Source;
	Source->Workbench = Workbench;
	TestFalse(TEXT("Automatic solving defaults to opt-in"), Source->bAutoSolve);
	if (!TestTrue(TEXT("Source owns a real queued CPU job before duplication"), Source->StartSolve()))
	{
		AddError(Source->StatusMessage);
		return false;
	}
	// Do not poll Source: even if the tiny worker finishes, its unpublished job remains queued.
	TestTrue(TEXT("Source remains queued until the game thread polls it"), Source->JobState == ECGHSolverJobState::Queued);
	TestTrue(TEXT("Source has a nondefault request identifier"), Source->JobId > 0);
	Source->LastComputeSeconds = 12.5; // A previous job's timing must also be reset on a new actor.
	// A nondefault editable value proves configuration is copied independently of runtime state.
	Source->Parameters.SolverBackend = ECGHSolverBackend::Docker;
	const uint64 EmptyRevision = SLM->GetPhasePatternRevision();
	const FName DuplicateName = MakeUniqueObjectName(Source->GetOuter(), Source->GetClass(), TEXT("CGHSolverDuplicateTest"));
	TStrongObjectPtr<ACGHSolverActor> Duplicate(Cast<ACGHSolverActor>(
		StaticDuplicateObject(Source, Source->GetOuter(), DuplicateName)));
	if (!TestNotNull(TEXT("A queued solver can be duplicated"), Duplicate.Get()))
	{
		return false;
	}
	const ACGHSolverActor* Defaults = GetDefault<ACGHSolverActor>();
	TestTrue(TEXT("Duplicate starts idle rather than inheriting queued status"), Duplicate->JobState == ECGHSolverJobState::Idle);
	TestEqual(TEXT("Duplicate restores the default status message"), Duplicate->StatusMessage, Defaults->StatusMessage);
	TestEqual(TEXT("Duplicate starts its own request numbering"), Duplicate->JobId, int64(0));
	TestEqual(TEXT("Duplicate does not inherit a previous computation duration"), Duplicate->LastComputeSeconds, 0.0);
	TestTrue(TEXT("Editable backend configuration survives duplication"), Duplicate->Parameters.SolverBackend == ECGHSolverBackend::Docker);
	TestTrue(TEXT("Editable algorithm survives duplication"), Duplicate->Parameters.Algorithm == Source->Parameters.Algorithm);
	TestTrue(TEXT("External workbench reference remains the same in a same-world actor duplicate"), Duplicate->Workbench == Workbench);
	TestFalse(TEXT("Default automatic solving remains disabled on the duplicate"), Duplicate->bAutoSolve);

	const FObjectPropertyBase* BackendProperty = FindFProperty<FObjectPropertyBase>(ACGHSolverActor::StaticClass(), TEXT("Backend"));
	if (TestNotNull(TEXT("Backend ownership is reflected"), BackendProperty))
	{
		TestNotNull(TEXT("Queued source has instantiated its backend"), BackendProperty->GetObjectPropertyValue_InContainer(Source));
		TestNull(TEXT("Duplicate does not copy the source backend instance"), BackendProperty->GetObjectPropertyValue_InContainer(Duplicate.Get()));
	}
	for (int32 Poll = 0; Poll < 3; ++Poll)
	{
		Duplicate->PollSolver();
	}
	TestTrue(TEXT("Polling an idle duplicate cannot inherit or launch a job"), Duplicate->JobState == ECGHSolverJobState::Idle);
	TestEqual(TEXT("Duplicate polling cannot publish the source's queued result"), SLM->GetPhasePatternRevision(), EmptyRevision);
	TestFalse(TEXT("No fabricated phase data is produced by duplication"), SLM->HasValidPhasePattern());

	// The source's original numerical snapshot is still valid after restoring the selector.
	Source->Parameters.SolverBackend = ECGHSolverBackend::CPU;
	const double Deadline = FPlatformTime::Seconds() + 10.0;
	do
	{
		Source->PollSolver();
		if (Source->JobState != ECGHSolverJobState::Queued && Source->JobState != ECGHSolverJobState::Running)
		{
			break;
		}
		FPlatformProcess::Sleep(0.001f); // Test-only yielding; production never waits for a solver.
	} while (FPlatformTime::Seconds() < Deadline);
	TestTrue(TEXT("Duplication leaves the source job able to publish successfully"), Source->JobState == ECGHSolverJobState::Ready);
	const uint64 SourceRevision = SLM->GetPhasePatternRevision();
	TestTrue(TEXT("Only the source publishes its solved pattern"), SourceRevision > EmptyRevision);
	Duplicate->PollSolver();
	TestEqual(TEXT("Source completion cannot trigger a second publication through the duplicate"), SLM->GetPhasePatternRevision(), SourceRevision);
	TestTrue(TEXT("Duplicate remains idle after the original job completes"), Duplicate->JobState == ECGHSolverJobState::Idle);
	TestEqual(TEXT("Duplicate retains independent unused request numbering"), Duplicate->JobId, int64(0));
	Scene.ForwardErrorMessages(this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
