#include "CGH/Reconstruction/CGHDockerReconstructionBackend.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Actors/CGHReconstructorActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CGH/Reconstruction/CGHReconstruction.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/AutomationCommon.h"
#include <cmath>
#include <limits>

namespace
{
	struct FCGHDockerReconstructionScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		ACGHReconstructionLightActor* Light = nullptr;
		ACGHObserverPlaneActor* Observer = nullptr;
		ACGHWorkbenchActor* Workbench = nullptr;
		ACGHReconstructorActor* Reconstructor = nullptr;

		bool Initialize(FAutomationTestBase& Test, int32 Port = 7000)
		{
			if (!CreateTestWorld(EWorldType::Game))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			SLM = TestWorld->SpawnActor<ACGHSLMActor>();
			Light = TestWorld->SpawnActor<ACGHReconstructionLightActor>();
			Observer = TestWorld->SpawnActor<ACGHObserverPlaneActor>();
			Workbench = TestWorld->SpawnActor<ACGHWorkbenchActor>();
			Reconstructor = TestWorld->SpawnActor<ACGHReconstructorActor>();
			if (!SLM || !Light || !Observer || !Workbench || !Reconstructor)
			{
				Test.AddError(TEXT("Could not spawn reconstruction fixture."));
				return false;
			}
			SLM->Parameters.ResolutionX = 4;
			SLM->Parameters.ResolutionY = 3;
			SLM->SetActorLocationAndRotation(FVector(10.0, -5.0, 3.0), FRotator(17.0, -21.0, 6.0));
			Light->SetActorRotation(SLM->GetActorRotation());
			Observer->Parameters.ResolutionX = 3;
			Observer->Parameters.ResolutionY = 2;
			Observer->SetActorLocationAndRotation(
				SLM->GetActorTransform().TransformPositionNoScale(FVector(40.0, 0.1, -0.05)), SLM->GetActorQuat());
			Workbench->SLM = SLM;
			Workbench->ReconstructionLight = Light;
			Workbench->ObserverPlane = Observer;
			Workbench->Reconstructor = Reconstructor;
			Reconstructor->Workbench = Workbench;
			Reconstructor->Parameters.ReconstructionBackend = ECGHReconstructionBackend::Docker;
			Reconstructor->Parameters.Docker.Port = Port;
			Reconstructor->Parameters.Docker.ConnectTimeoutSeconds = 0.5;
			Reconstructor->Parameters.Docker.RequestTimeoutSeconds = 2.0;
			return SetPhase(Test, 0.4) && Sentinel(Test);
		}

		FCGHReconstructionInput CaptureInput(FAutomationTestBase& Test)
		{
			FCGHReconstructionInput Input;
			FString Error;
			if (!Workbench->CaptureReconstructionInput(Input, Error)) { Test.AddError(Error); }
			Input.Pattern = SLM->GetPhasePattern();
			return Input;
		}

		bool SetPhase(FAutomationTestBase& Test, double Phase)
		{
			FCGHSLMPhasePattern Pattern;
			Pattern.ResolutionX = SLM->Parameters.ResolutionX;
			Pattern.ResolutionY = SLM->Parameters.ResolutionY;
			Pattern.PhaseRad.Init(Phase, Pattern.ResolutionX * Pattern.ResolutionY);
			return Test.TestTrue(TEXT("Publish fixture SLM phase"), SLM->SetPhasePattern(MoveTemp(Pattern)));
		}

		bool Sentinel(FAutomationTestBase& Test)
		{
			FCGHComplexField Field;
			Field.ResolutionX = Observer->Parameters.ResolutionX;
			Field.ResolutionY = Observer->Parameters.ResolutionY;
			FCGHComplexSample Sample;
			Sample.Real = 0.125;
			Sample.Imaginary = -0.75;
			Field.Samples.Init(Sample, Field.ResolutionX * Field.ResolutionY);
			return Test.TestTrue(TEXT("Publish fixture observer field"), Observer->SetComplexField(MoveTemp(Field)));
		}
	};

	bool WaitForDockerReconstructor(FAutomationTestBase& Test, ACGHReconstructorActor& Actor, FTestWorldWrapper* Scene = nullptr)
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		do
		{
			if (Scene) Scene->TickTestWorld();
			else Actor.PollReconstructor();
			if (Actor.JobState != ECGHReconstructionJobState::Queued && Actor.JobState != ECGHReconstructionJobState::Running)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		} while (FPlatformTime::Seconds() < Deadline);
		Test.AddError(FString::Printf(TEXT("Reconstructor did not settle: %s"), *Actor.StatusMessage));
		Actor.CancelReconstruction();
		return false;
	}
	/** Independent CGHV 1.4 bytes expose decoder errors that a shared encoder could conceal. */
	struct FCGHReconstructionPeer
	{
		ISocketSubsystem* Subsystem = nullptr;
		FSocket* Listener = nullptr;
		TFuture<bool> Worker;
		std::atomic<bool> bStop{false};
		int32 Port = 0;

		~FCGHReconstructionPeer()
		{
			bStop.store(true, std::memory_order_relaxed);
			if (Worker.IsValid())
			{
				Worker.Wait();
			}
			if (Listener)
			{
				Listener->Close();
				Subsystem->DestroySocket(Listener);
			}
		}

		bool Transfer(FSocket& Socket, uint8* Bytes, int32 Size, bool bWrite, double Deadline)
		{
			int32 Offset = 0;
			while (Offset < Size && !bStop.load(std::memory_order_relaxed) && FPlatformTime::Seconds() < Deadline)
			{
				if (!Socket.Wait(bWrite ? ESocketWaitConditions::WaitForWrite : ESocketWaitConditions::WaitForRead,
					FTimespan::FromMilliseconds(10)))
				{
					continue;
				}
				int32 Count = 0;
				// Seven-byte writes deliberately split both the header and binary64 real/imaginary samples.
				const bool bTransferred = bWrite
					? Socket.Send(Bytes + Offset, FMath::Min(Size - Offset, 7), Count)
					: Socket.Recv(Bytes + Offset, Size - Offset, Count, ESocketReceiveFlags::None);
				if (!bTransferred)
				{
					return false;
				}
				Offset += Count;
			}
			return Offset == Size;
		}

		bool Start(FAutomationTestBase& Test, int32 ResponseCase, int32 DelayMilliseconds = 0)
		{
			Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (!Test.TestNotNull(TEXT("Scripted TCP peer has a socket subsystem"), Subsystem))
			{
				return false;
			}
			Listener = Subsystem->CreateSocket(NAME_Stream, TEXT("CGH malformed response fixture"), FNetworkProtocolTypes::IPv4);
			const TSharedRef<FInternetAddr> Address = Subsystem->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
			Address->SetIp(0x7f000001);
			Address->SetPort(0);
			if (!Listener || !Listener->SetNonBlocking(true) || !Listener->Bind(*Address) || !Listener->Listen(1))
			{
				Test.AddError(TEXT("Could not bind the scripted TCP peer to an ephemeral loopback port."));
				return false;
			}
			Port = Listener->GetPortNo();
			Worker = Async(EAsyncExecution::Thread, [this, ResponseCase, DelayMilliseconds]()
			{
				const double Deadline = FPlatformTime::Seconds() + 4.0;
				FSocket* Client = nullptr;
				while (!Client && !bStop.load(std::memory_order_relaxed) && FPlatformTime::Seconds() < Deadline)
				{
					if (Listener->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(10)))
					{
						Client = Listener->Accept(TEXT("CGH scripted peer client"));
					}
				}
				if (!Client)
				{
					return false;
				}
				Client->SetNonBlocking(true);
				Client->SetNoDelay(true);
				bool bReceivedRequest = false;
				uint8 RequestHeader[32];
				if (Transfer(*Client, RequestHeader, 32, false, Deadline))
				{
					uint64 PayloadSize = 0;
					for (int32 Byte = 24; Byte < 32; ++Byte)
					{
						PayloadSize = (PayloadSize << 8) | RequestHeader[Byte];
					}
					if (PayloadSize > 0 && PayloadSize < 4096)
					{
						TArray<uint8> RequestPayload;
						RequestPayload.SetNumUninitialized(static_cast<int32>(PayloadSize));
						bReceivedRequest = Transfer(*Client, RequestPayload.GetData(), RequestPayload.Num(), false, Deadline)
							&& RequestHeader[5] == 1 && RequestHeader[7] == 4 && RequestHeader[9] == 5;
					}
				}
				if (bReceivedRequest)
				{
					const double ReplyTime = FPlatformTime::Seconds() + DelayMilliseconds / 1000.0;
					while (!bStop.load(std::memory_order_relaxed) && FPlatformTime::Seconds() < ReplyTime)
					{
						FPlatformProcess::Sleep(0.005f);
					}
					// Independently encoded 1.4 complex result: 32-byte metadata then six real/imaginary pairs.
					TArray<uint8> Reply;
					Reply.Init(0, 32 + 32 + 6 * 16);
					Reply[0] = 'C'; Reply[1] = 'G'; Reply[2] = 'H'; Reply[3] = 'V';
					Reply[5] = 1; // major 1
					Reply[7] = 4; // minor 4
					Reply[9] = 6; // ReconstructionResult
					FMemory::Memcpy(Reply.GetData() + 16, RequestHeader + 16, 8);
					Reply[31] = 32 + 6 * 16;
					Reply[35] = 3; // ReconstructionSuccess
					Reply[39] = 1; // ExpPositiveIKR
					Reply[43] = 3; Reply[47] = 2;
					Reply[63] = 6;
					for (int32 Index = 0; Index < 6; ++Index)
					{
						const double Values[] = {Index + 0.125, -(Index + 0.75)};
						for (int32 Component = 0; Component < 2; ++Component)
						{
							uint64 Bits;
							FMemory::Memcpy(&Bits, &Values[Component], 8);
							for (int32 Byte = 0; Byte < 8; ++Byte)
							{
								Reply[64 + Index * 16 + Component * 8 + Byte] = static_cast<uint8>(Bits >> ((7 - Byte) * 8));
							}
						}
					}
					switch (ResponseCase)
					{
					case 1: Reply[16] ^= 1; break; // Different, nonzero request identity.
					case 2: Reply[39] = 2; break; // Unknown convention.
					case 3: Reply[43] = 2; Reply[47] = 3; break; // Same product, wrong axes.
					case 4: Reply[5] = 2; break; // Unsupported protocol major.
					case 5: Reply[64] = 0x7f; Reply[65] = 0xf8; break; // Quiet NaN phase.
					case 6: Reply.SetNum(72); break; // Truncated body, unchanged advertised length.
					case 7: Reply[29] = 1; break; // Payload exceeds the requested output size.
					case 8: Reply[48] = 0x7f; Reply[49] = 0xf0; break; // Infinite compute duration.
					case 9: Reply[7] = 5; break; // Unsupported future protocol minor.
					case 10: Reply[35] = 1; break; // A solver success is not reconstruction success.
					case 11: Reply[9] = 2; break; // Solver Result instead of ReconstructionResult.
					case 12: Reply[72] = 0x7f; Reply[73] = 0xf0; break; // Infinite imaginary component.
					case 13: Reply[63] = 5; break; // Sample count mismatches dimensions.
					case 14:
						Reply.SetNumZeroed(40); Reply[9] = 3; Reply[31] = 8;
						for (int32 Byte = 32; Byte < 40; ++Byte) Reply[Byte] = 0;
						Reply[35] = 4; Reply[36] = 't'; Reply[37] = 'e'; Reply[38] = 's'; Reply[39] = 't';
						break; // Well-formed remote error.
					}
					Transfer(*Client, Reply.GetData(), Reply.Num(), true, Deadline);
				}
				Client->Shutdown(ESocketShutdownMode::ReadWrite);
				Client->Close();
				Subsystem->DestroySocket(Client);
				return bReceivedRequest;
			});
			return true;
		}
	};

	bool WaitForDockerReconstructionJob(FAutomationTestBase& Test, const TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe>& Job)
	{
		if (!Test.TestTrue(TEXT("Docker reconstruction returns an independent mailbox"), Job.IsValid())) return false;
		const double Deadline = FPlatformTime::Seconds() + 8.0;
		while (!Job->bFinished.load(std::memory_order_acquire))
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Job->bCancelRequested.store(true, std::memory_order_relaxed);
				Test.AddError(TEXT("Docker reconstruction worker did not finish within the fixture deadline."));
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	}

	bool CheckReconstructionParity(FAutomationTestBase& Test, const FCGHComplexField& Actual, const FCGHComplexField& Expected)
	{
		if (!Test.TestTrue(TEXT("CUDA returns a complete complex field"), Actual.IsValid())
			|| !Test.TestEqual(TEXT("CUDA preserves observer width"), Actual.ResolutionX, Expected.ResolutionX)
			|| !Test.TestEqual(TEXT("CUDA preserves observer height"), Actual.ResolutionY, Expected.ResolutionY)
			|| !Test.TestEqual(TEXT("CUDA preserves sample count"), Actual.Samples.Num(), Expected.Samples.Num())) return false;
		double Scale = 0.0;
		for (const FCGHComplexSample& Sample : Expected.Samples) Scale = FMath::Max(Scale, std::hypot(Sample.Real, Sample.Imaginary));
		const double Tolerance = FMath::Max(1.e-14, Scale * 1.e-7);
		for (int32 Index = 0; Index < Expected.Samples.Num(); ++Index)
		{
			Test.TestEqual(FString::Printf(TEXT("CUDA real sample %d matches CPU"), Index), Actual.Samples[Index].Real, Expected.Samples[Index].Real, Tolerance);
			Test.TestEqual(FString::Printf(TEXT("CUDA imaginary sample %d matches CPU"), Index), Actual.Samples[Index].Imaginary, Expected.Samples[Index].Imaginary, Tolerance);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerReconstructionMalformedTest,
	"CGH.DockerReconstruction.MalformedPeerResponses", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerReconstructionMalformedTest::RunTest(const FString& Parameters)
{
	const TCHAR* Cases[] = {TEXT("valid fragmented reply"), TEXT("wrong job identity"), TEXT("wrong convention"),
		TEXT("transposed dimensions"), TEXT("unsupported major"), TEXT("NaN real component"), TEXT("truncated body"),
		TEXT("oversized payload"), TEXT("infinite compute duration"), TEXT("unsupported minor"), TEXT("solver success status"),
		TEXT("solver response type"), TEXT("infinite imaginary component"), TEXT("mismatched sample count"), TEXT("server error")};
	for (int32 ResponseCase = 0; ResponseCase < UE_ARRAY_COUNT(Cases); ++ResponseCase)
	{
		FCGHReconstructionPeer Peer;
		FCGHDockerReconstructionScene Scene;
		if (!Peer.Start(*this, ResponseCase) || !Scene.Initialize(*this, Peer.Port)) return false;
		const uint64 Before = Scene.Observer->GetComplexFieldRevision();
		TestTrue(TEXT("Actor queues a real reconstruction request"), Scene.Reconstructor->StartReconstruction());
		if (!WaitForDockerReconstructor(*this, *Scene.Reconstructor)) return false;
		TestTrue(TEXT("Peer receives a complete CGHV 1.4 reconstruction request"), Peer.Worker.Get());
		if (ResponseCase == 0)
		{
			if (!TestTrue(TEXT("Fragmented complex response reaches Ready"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready))
			{
				AddError(Scene.Reconstructor->StatusMessage); return false;
			}
			TestEqual(TEXT("Complete valid field advances observer revision"), Scene.Observer->GetComplexFieldRevision(), Before + 1);
			if (!TestEqual(TEXT("All six complex samples publish"), Scene.Observer->GetComplexField().Samples.Num(), 6)) return false;
			for (int32 Index = 0; Index < 6; ++Index)
			{
				TestEqual(TEXT("Real components retain float64 precision and order"), Scene.Observer->GetComplexField().Samples[Index].Real, Index + 0.125);
				TestEqual(TEXT("Imaginary components retain float64 precision and order"), Scene.Observer->GetComplexField().Samples[Index].Imaginary, -(Index + 0.75));
			}
		}
		else
		{
			TestTrue(FString::Printf(TEXT("Reject %s"), Cases[ResponseCase]), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Failed);
			TestFalse(TEXT("Rejected response provides a diagnostic"), Scene.Reconstructor->StatusMessage.IsEmpty());
			TestEqual(TEXT("Rejected response preserves prior observer revision"), Scene.Observer->GetComplexFieldRevision(), Before);
			TestEqual(TEXT("Rejected response preserves real data"), Scene.Observer->GetComplexField().Samples.Last().Real, 0.125);
			TestEqual(TEXT("Rejected response preserves imaginary data"), Scene.Observer->GetComplexField().Samples.Last().Imaginary, -0.75);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerReconstructionValidationTest,
	"CGH.DockerReconstruction.InvalidSettingsAndInput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerReconstructionValidationTest::RunTest(const FString& Parameters)
{
	FCGHDockerReconstructionScene Scene;
	if (!Scene.Initialize(*this)) return false;
	UCGHDockerReconstructionBackend* Backend = NewObject<UCGHDockerReconstructionBackend>();
	for (int32 Case = 0; Case < 10; ++Case)
	{
		Backend->Settings = FCGHDockerSolverSettings();
		FCGHReconstructionInput Input = Scene.CaptureInput(*this);
		switch (Case)
		{
		case 0: Backend->Settings.Address = TEXT("not-an-address"); break;
		case 1: Backend->Settings.Port = 0; break;
		case 2: Backend->Settings.Port = 65536; break;
		case 3: Backend->Settings.ConnectTimeoutSeconds = 0.0; break;
		case 4: Backend->Settings.RequestTimeoutSeconds = std::numeric_limits<double>::quiet_NaN(); break;
		case 5: Input.Pattern.PhaseRad[0] = std::numeric_limits<double>::infinity(); break;
		case 6: Input.Pattern.ResolutionX += 1; break;
		case 7: Input.ObserverPlane.PositionSLMM.X = -1.0; break;
		case 8: Input.Mode = static_cast<ECGHReconstructionMode>(255); break;
		case 9: Input.PropagationConvention = static_cast<ECGHPropagationConvention>(127); break;
		}
		const auto Job = Backend->Submit(MoveTemp(Input));
		Backend->Settings = FCGHDockerSolverSettings();
		if (!WaitForDockerReconstructionJob(*this, Job)) return false;
		TestFalse(FString::Printf(TEXT("Invalid settings or input case %d fails"), Case), Job->Result.bSucceeded);
		TestFalse(TEXT("Failure provides an actionable error"), Job->Result.Error.IsEmpty());
		TestTrue(TEXT("Failed reconstruction has no partial complex array"), Job->Result.Field.Samples.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerReconstructionCancellationTest,
	"CGH.DockerReconstruction.TimeoutCancellationAndSettingsSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerReconstructionCancellationTest::RunTest(const FString& Parameters)
{
	for (int32 Case = 0; Case < 3; ++Case)
	{
		FCGHReconstructionPeer Peer;
		FCGHDockerReconstructionScene Scene;
		if (!Peer.Start(*this, 0, 350) || !Scene.Initialize(*this, Peer.Port)) return false;
		UCGHDockerReconstructionBackend* Backend = NewObject<UCGHDockerReconstructionBackend>();
		Backend->Settings = Scene.Reconstructor->Parameters.Docker;
		if (Case == 0) Backend->Settings.RequestTimeoutSeconds = 0.03;
		const double Started = FPlatformTime::Seconds();
		const auto Job = Backend->Submit(Scene.CaptureInput(*this));
		// Every case edits the UObject immediately; the submitted worker must retain its settings snapshot.
		Backend->Settings.Port = 0;
		Backend->Settings.RequestTimeoutSeconds = 0.001;
		if (Case == 1)
		{
			FPlatformProcess::Sleep(0.03f);
			Job->bCancelRequested.store(true, std::memory_order_relaxed);
		}
		if (!WaitForDockerReconstructionJob(*this, Job)) return false;
		if (Case < 2)
		{
			TestFalse(TEXT("Timeout and cancellation cannot succeed"), Job->Result.bSucceeded);
			TestTrue(TEXT("Timeout and cancellation expose no partial field"), Job->Result.Field.Samples.IsEmpty());
			TestTrue(TEXT("Cancellation/deadline bound worker completion"), FPlatformTime::Seconds() - Started < 1.0);
			TestTrue(TEXT("Termination reason is explicit"), Job->Result.Error.Contains(Case == 0 ? TEXT("timed out") : TEXT("cancelled")));
		}
		else
		{
			TestTrue(TEXT("An unchanged job snapshot completes despite backend setting edits"), Job->Result.bSucceeded);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerReconstructionStaleTest,
	"CGH.DockerReconstruction.StaleEndpointAndAutomaticRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerReconstructionStaleTest::RunTest(const FString& Parameters)
{
	for (int32 Mutation = 0; Mutation < 4; ++Mutation)
	{
		FCGHReconstructionPeer Peer;
		FCGHDockerReconstructionScene Scene;
		if (!Peer.Start(*this, 0, 100) || !Scene.Initialize(*this, Peer.Port)) return false;
		const uint64 Before = Scene.Observer->GetComplexFieldRevision();
		TestTrue(TEXT("Remote job starts before transport settings change"), Scene.Reconstructor->StartReconstruction());
		switch (Mutation)
		{
		case 0: Scene.Reconstructor->Parameters.Docker.Address = TEXT("127.0.0.2"); break;
		case 1: Scene.Reconstructor->Parameters.Docker.Port += 1; break;
		case 2: Scene.Reconstructor->Parameters.Docker.ConnectTimeoutSeconds += 0.5; break;
		case 3: Scene.Reconstructor->Parameters.Docker.RequestTimeoutSeconds += 0.5; break;
		}
		if (!WaitForDockerReconstructor(*this, *Scene.Reconstructor)) return false;
		TestTrue(TEXT("Obsolete transport settings reject the result"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Failed);
		TestEqual(TEXT("Stale endpoint cannot overwrite observer field"), Scene.Observer->GetComplexFieldRevision(), Before);
		TestTrue(TEXT("Stale rejection explains a changed input"), Scene.Reconstructor->StatusMessage.Contains(TEXT("changed")));
	}
	FCGHReconstructionPeer OldPeer, NewPeer;
	FCGHDockerReconstructionScene Scene;
	if (!OldPeer.Start(*this, 0, 300) || !NewPeer.Start(*this, 0) || !Scene.Initialize(*this, OldPeer.Port)) return false;
	Scene.Reconstructor->bAutoReconstruct = true;
	Scene.Reconstructor->PollReconstructor();
	const int64 OldJob = Scene.Reconstructor->JobId;
	Scene.Reconstructor->Parameters.Docker.Port = NewPeer.Port;
	Scene.Reconstructor->PollReconstructor();
	TestTrue(TEXT("Endpoint edit automatically queues a new job"), Scene.Reconstructor->JobId > OldJob);
	if (!WaitForDockerReconstructor(*this, *Scene.Reconstructor)) return false;
	TestTrue(TEXT("Latest endpoint recovers to Ready"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	const int64 FinishedJob = Scene.Reconstructor->JobId;
	Scene.Reconstructor->PollReconstructor();
	TestEqual(TEXT("Unchanged endpoint does not resubmit repeatedly"), Scene.Reconstructor->JobId, FinishedJob);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerReconstructionCudaParityTest,
	"CGH.DockerReconstruction.CudaParityAndObserverPublication", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerReconstructionCudaParityTest::RunTest(const FString& Parameters)
{
	int32 Port = 0;
	if (!FParse::Value(FCommandLine::Get(), TEXT("CGHCudaTestPort="), Port))
	{
		AddWarning(TEXT("SKIPPED real GPU reconstruction parity; supply -CGHCudaTestPort=<published CUDA server port> to run it."));
		return true;
	}
	if (!TestTrue(TEXT("CGHCudaTestPort is valid"), Port > 0 && Port <= 65535)) return false;
	FCGHDockerReconstructionScene Scene;
	if (!Scene.Initialize(*this, Port)) return false;
	UCGHDockerReconstructionBackend* Backend = NewObject<UCGHDockerReconstructionBackend>();
	Backend->Settings = Scene.Reconstructor->Parameters.Docker;
	Backend->Settings.RequestTimeoutSeconds = 5.0;
	std::atomic<bool> Cancel{false};
	for (int32 Case = 0; Case < 6; ++Case)
	{
		FCGHReconstructionInput Input = Scene.CaptureInput(*this);
		for (int32 Index = 0; Index < Input.Pattern.PhaseRad.Num(); ++Index)
		{
			Input.Pattern.PhaseRad[Index] = (Index * 7 % 13) * 0.137 - 0.4 + (Index % 2) * UE_DOUBLE_TWO_PI;
		}
		switch (Case)
		{
		case 1:
			Input.Light.SourceType = ECGHSourceType::PointSource;
			Input.Light.PositionSLMM = FVector(-0.08, 0.003, -0.005);
			break;
		case 2:
			Input.ObserverPlane.RotationSLM = FQuat(FVector(0.3, -0.4, 0.5).GetSafeNormal(), 0.31);
			Input.Light.DirectionSLM = FVector(1.0, 0.2, -0.3).GetSafeNormal();
			break;
		case 3: Input.Light.Amplitude = 0.0; break;
		case 4:
			Input.Light.InitialPhaseRad = 1.e10;
			for (double& Phase : Input.Pattern.PhaseRad) Phase += 1.e10;
			break;
		case 5:
			Input.SLM.ResolutionX = 1; Input.SLM.ResolutionY = 1;
			Input.SLM.ActiveWidthM = Input.SLM.PixelPitchXM; Input.SLM.ActiveHeightM = Input.SLM.PixelPitchYM;
			Input.Pattern.ResolutionX = 1; Input.Pattern.ResolutionY = 1; Input.Pattern.PhaseRad = {0.73};
			break;
		}
		const FCGHReconstructionResult Reference = CGHReconstruction::Reconstruct(Input, Cancel);
		if (!TestTrue(TEXT("CPU reference reconstruction succeeds"), Reference.bSucceeded)) { AddError(Reference.Error); return false; }
		const auto Job = Backend->Submit(MoveTemp(Input));
		if (!WaitForDockerReconstructionJob(*this, Job)) return false;
		if (!TestTrue(FString::Printf(TEXT("Live CUDA reconstruction case %d succeeds"), Case), Job->Result.bSucceeded)) { AddError(Job->Result.Error); return false; }
		CheckReconstructionParity(*this, Job->Result.Field, Reference.Field);
	}
	const FCGHReconstructionResult Reference = CGHReconstruction::Reconstruct(Scene.CaptureInput(*this), Cancel);
	const uint64 PhaseRevision = Scene.SLM->GetPhasePatternRevision();
	TestTrue(TEXT("Actor queues live CUDA reconstruction"), Scene.Reconstructor->StartReconstruction());
	if (!WaitForDockerReconstructor(*this, *Scene.Reconstructor)) return false;
	if (!TestTrue(TEXT("Live CUDA result publishes through the observer actor"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready))
	{
		AddError(Scene.Reconstructor->StatusMessage); return false;
	}
	CheckReconstructionParity(*this, Scene.Observer->GetComplexField(), Reference.Field);
	TestEqual(TEXT("Remote reconstruction leaves the source SLM phase unchanged"), Scene.SLM->GetPhasePatternRevision(), PhaseRevision);
	TestTrue(TEXT("Docker publication reports finite computation duration"), FMath::IsFinite(Scene.Reconstructor->LastComputeSeconds) && Scene.Reconstructor->LastComputeSeconds >= 0.0);
	Scene.Reconstructor->Parameters.ReconstructionBackend = ECGHReconstructionBackend::CPU;
	TestTrue(TEXT("CPU backend remains available after Docker reconstruction"), Scene.Reconstructor->StartReconstruction());
	if (!WaitForDockerReconstructor(*this, *Scene.Reconstructor)) return false;
	TestTrue(TEXT("CPU backend switch reaches Ready"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	return true;
}

#endif
