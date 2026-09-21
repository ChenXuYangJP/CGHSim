#include "CGH/Solver/CGHDockerSolverBackend.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHSolverActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/AutomationCommon.h"

#include <limits>

namespace
{
	struct FCGHDockerTestScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		ACGHReconstructionLightActor* Light = nullptr;
		ACGHTargetActor* Target = nullptr;
		ACGHWorkbenchActor* Workbench = nullptr;
		ACGHSolverActor* Solver = nullptr;

		bool Initialize(FAutomationTestBase& Test, int32 Port = 7000)
		{
			if (!CreateTestWorld(EWorldType::Game))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			SLM = TestWorld->SpawnActor<ACGHSLMActor>();
			Light = TestWorld->SpawnActor<ACGHReconstructionLightActor>();
			Target = TestWorld->SpawnActor<ACGHTargetActor>();
			Workbench = TestWorld->SpawnActor<ACGHWorkbenchActor>();
			Solver = TestWorld->SpawnActor<ACGHSolverActor>();
			if (!SLM || !Light || !Target || !Workbench || !Solver)
			{
				Test.AddError(TEXT("Could not spawn Docker solver fixture actors."));
				return false;
			}
			SLM->Parameters.ResolutionX = 5;
			SLM->Parameters.ResolutionY = 3;
			SLM->Parameters.PixelPitchXUm = 80.0;
			SLM->Parameters.PixelPitchYUm = 110.0;
			SLM->ClearPhasePattern();
			Light->Parameters.WavelengthNm = 633.123456789;
			Target->SetActorLocation(FVector(50.0, 1.5, -2.4));
			Target->Parameters.InitialPhaseRad = 0.73;
			Workbench->SLM = SLM;
			Workbench->ReconstructionLight = Light;
			Workbench->Targets = {Target};
			Workbench->Solver = Solver;
			Solver->Workbench = Workbench;
			Solver->Parameters.SolverBackend = ECGHSolverBackend::Docker;
			Solver->Parameters.Docker.Address = TEXT("127.0.0.1");
			Solver->Parameters.Docker.Port = Port;
			Solver->Parameters.Docker.ConnectTimeoutSeconds = 2.0;
			Solver->Parameters.Docker.RequestTimeoutSeconds = 3.0;
			return PublishSentinel(Test);
		}

		bool PublishSentinel(FAutomationTestBase& Test, double Phase = 0.125)
		{
			FCGHSLMPhasePattern Pattern;
			Pattern.ResolutionX = SLM->Parameters.ResolutionX;
			Pattern.ResolutionY = SLM->Parameters.ResolutionY;
			Pattern.PhaseRad.Init(Phase, Pattern.ResolutionX * Pattern.ResolutionY);
			return Test.TestTrue(TEXT("Fixture publishes a valid previous pattern"), SLM->SetPhasePattern(MoveTemp(Pattern)));
		}

		FCGHSolverInput CaptureInput()
		{
			Workbench->UpdateSceneDescription();
			FCGHSolverInput Input;
			Input.Scene = Workbench->SceneDescription;
			return Input;
		}
	};

	bool WaitForDockerActor(FAutomationTestBase& Test, ACGHSolverActor& Solver)
	{
		const double Deadline = FPlatformTime::Seconds() + 8.0;
		do
		{
			Solver.PollSolver();
			if (Solver.JobState != ECGHSolverJobState::Queued && Solver.JobState != ECGHSolverJobState::Running)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		} while (FPlatformTime::Seconds() < Deadline);
		Test.AddError(FString::Printf(TEXT("Docker solver did not settle: %s"), *Solver.StatusMessage));
		Solver.CancelSolve();
		return false;
	}

	bool WaitForDockerJob(FAutomationTestBase& Test, const TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe>& Job)
	{
		if (!Test.TestTrue(TEXT("Docker backend returns an owned job mailbox"), Job.IsValid()))
		{
			return false;
		}
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (!Job->bFinished.load(std::memory_order_acquire))
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Job->bCancelRequested.store(true, std::memory_order_relaxed);
				Test.AddError(TEXT("Docker worker did not finish its job mailbox."));
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	}

	/** The executable is explicit: regular CGH automation never depends on Docker being installed. */
	bool GetDockerTestExecutable(FAutomationTestBase& Test, FString& Executable)
	{
		if (!FParse::Value(FCommandLine::Get(), TEXT("CGHDockerTestServer="), Executable))
		{
			Test.AddWarning(TEXT("SKIPPED standalone TCP integration; supply -CGHDockerTestServer=<absolute path to cgh_v100_server> to run it."));
			return false;
		}
		if (!FPaths::FileExists(Executable))
		{
			Test.AddError(FString::Printf(TEXT("CGHDockerTestServer does not exist: %s"), *Executable));
			return false;
		}
		return true;
	}

	/** Each test owns a Linux server listening on an OS-selected port, including all cleanup paths. */
	struct FCGHDockerTestServer
	{
		FProcHandle Process;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		int32 Port = 0;

		~FCGHDockerTestServer() { Stop(); }

		void Stop()
		{
			if (Process.IsValid())
			{
				if (FPlatformProcess::IsProcRunning(Process))
				{
					FPlatformProcess::TerminateProc(Process);
				}
				FPlatformProcess::WaitForProc(Process);
				FPlatformProcess::CloseProc(Process);
			}
			if (ReadPipe || WritePipe)
			{
				FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
				ReadPipe = nullptr;
				WritePipe = nullptr;
			}
		}

		bool Start(FAutomationTestBase& Test, const FString& Executable, int32 DelayMilliseconds)
		{
			if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
			{
				Test.AddError(TEXT("Could not create the standalone server output pipe."));
				return false;
			}
			const FString Arguments = FString::Printf(TEXT("--port 0 --delay-ms %d"), DelayMilliseconds);
			Process = FPlatformProcess::CreateProc(*Executable, *Arguments, false, true, true,
				nullptr, 0, nullptr, WritePipe);
			if (!Process.IsValid())
			{
				Test.AddError(TEXT("Could not launch CGHDockerTestServer."));
				return false;
			}
			FString Output;
			const double Deadline = FPlatformTime::Seconds() + 5.0;
			do
			{
				Output += FPlatformProcess::ReadPipe(ReadPipe);
				if (FParse::Value(*Output, TEXT("LISTENING "), Port) && Port > 0 && Port <= 65535)
				{
					return true;
				}
				if (!FPlatformProcess::IsProcRunning(Process))
				{
					break;
				}
				FPlatformProcess::Sleep(0.005f);
			} while (FPlatformTime::Seconds() < Deadline);
			Test.AddError(FString::Printf(TEXT("Standalone server did not report its listening port: %s"), *Output));
			return false;
		}
	};


	/** Independent CGHV 1.0 bytes expose decoder errors that a shared encoder could conceal. */
	struct FCGHScriptedPeer
	{
		ISocketSubsystem* Subsystem = nullptr;
		FSocket* Listener = nullptr;
		TFuture<bool> Worker;
		std::atomic<bool> bStop{false};
		int32 Port = 0;

		~FCGHScriptedPeer()
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
				// Seven-byte writes deliberately split both the header and binary64 phase samples.
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

		bool Start(FAutomationTestBase& Test, int32 ResponseCase)
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
			Worker = Async(EAsyncExecution::Thread, [this, ResponseCase]()
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
						bReceivedRequest = Transfer(*Client, RequestPayload.GetData(), RequestPayload.Num(), false, Deadline);
					}
				}
				if (bReceivedRequest)
				{
					// 32-byte header, 32-byte result metadata, 15 big-endian binary64 zeros.
					TArray<uint8> Reply;
					Reply.Init(0, 32 + 32 + 15 * 8);
					Reply[0] = 'C'; Reply[1] = 'G'; Reply[2] = 'H'; Reply[3] = 'V';
					Reply[5] = 1; // major 1
					Reply[9] = 2; // Result
					FMemory::Memcpy(Reply.GetData() + 16, RequestHeader + 16, 8);
					Reply[31] = 32 + 15 * 8;
					Reply[35] = 1; // DummySuccess
					Reply[39] = 1; // ExpPositiveIKR
					Reply[43] = 5; Reply[47] = 3;
					Reply[63] = 15;
					switch (ResponseCase)
					{
					case 1: Reply[16] ^= 1; break; // Different, nonzero request identity.
					case 2: Reply[39] = 2; break; // Unknown convention.
					case 3: Reply[43] = 3; Reply[47] = 5; break; // Same product, wrong axes.
					case 4: Reply[5] = 2; break; // Unsupported protocol major.
					case 5: Reply[64] = 0x7f; Reply[65] = 0xf8; break; // Quiet NaN phase.
					case 6: Reply.SetNum(72); break; // Truncated body, unchanged advertised length.
					case 7: Reply[29] = 1; break; // Payload exceeds the requested output size.
					case 8: Reply[48] = 0x7f; Reply[49] = 0xf0; break; // Infinite compute duration.
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

	bool CheckDummyPattern(FAutomationTestBase& Test, const FCGHDockerTestScene& Scene)
	{
		if (!Test.TestTrue(TEXT("TCP result reaches Ready through PollSolver"), Scene.Solver->JobState == ECGHSolverJobState::Ready)
			|| !Test.TestTrue(TEXT("TCP result is a valid SLM phase pattern"), Scene.SLM->HasValidPhasePattern()))
		{
			Test.AddError(Scene.Solver->StatusMessage);
			return false;
		}
		const FCGHSLMPhasePattern& Pattern = Scene.SLM->GetPhasePattern();
		Test.TestEqual(TEXT("Asymmetric width survives TCP serialization"), Pattern.ResolutionX, 5);
		Test.TestEqual(TEXT("Asymmetric height survives TCP serialization"), Pattern.ResolutionY, 3);
		if (!Test.TestEqual(TEXT("The complete row-major phase array is received"), Pattern.PhaseRad.Num(), 15))
		{
			return false;
		}
		for (int32 Index = 0; Index < Pattern.PhaseRad.Num(); ++Index)
		{
			const int32 Column = Index % Pattern.ResolutionX;
			const int32 Row = Index / Pattern.ResolutionX;
			const double Expected = 2.0 * UE_DOUBLE_PI * ((Column + 3 * Row) % 256) / 256.0;
			Test.TestEqual(FString::Printf(TEXT("Dummy phase sample %d preserves float64 and row-major order"), Index),
				Pattern.PhaseRad[Index], Expected, 1.0e-12);
		}
		Test.TestTrue(TEXT("Remote compute duration is finite and nonnegative"),
			FMath::IsFinite(Scene.Solver->LastComputeSeconds) && Scene.Solver->LastComputeSeconds >= 0.0);
		Test.TestTrue(TEXT("The published status identifies the dummy result"), Scene.Solver->StatusMessage.Contains(TEXT("dummy")));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerInvalidSettingsTest,
	"CGH.DockerBackend.InvalidSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerInvalidSettingsTest::RunTest(const FString& Parameters)
{
	FCGHDockerTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	UCGHDockerSolverBackend* Backend = NewObject<UCGHDockerSolverBackend>();
	for (int32 InvalidCase = 0; InvalidCase < 5; ++InvalidCase)
	{
		Backend->Settings = FCGHDockerSolverSettings();
		switch (InvalidCase)
		{
		case 0: Backend->Settings.Address = TEXT("invalid-address"); break;
		case 1: Backend->Settings.Port = 0; break;
		case 2: Backend->Settings.Port = 65536; break;
		case 3: Backend->Settings.ConnectTimeoutSeconds = 0.0; break;
		case 4: Backend->Settings.RequestTimeoutSeconds = std::numeric_limits<double>::quiet_NaN(); break;
		}
		const auto Job = Backend->Submit(Scene.CaptureInput());
		if (!WaitForDockerJob(*this, Job))
		{
			return false;
		}
		TestFalse(FString::Printf(TEXT("Invalid transport setting %d fails"), InvalidCase), Job->Result.bSucceeded);
		TestFalse(TEXT("Invalid transport settings return a diagnostic"), Job->Result.Error.IsEmpty());
		TestTrue(TEXT("Failed jobs never expose a partial phase array"), Job->Result.Pattern.PhaseRad.IsEmpty());
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerMalformedResponseTest,
	"CGH.DockerBackend.MalformedPeerResponses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerMalformedResponseTest::RunTest(const FString& Parameters)
{
	const TCHAR* Cases[] = {TEXT("valid fragmented reply"), TEXT("wrong job ID"), TEXT("wrong convention"),
		TEXT("transposed dimensions"), TEXT("unsupported version"), TEXT("NaN phase"),
		TEXT("truncated body"), TEXT("oversized payload"), TEXT("infinite compute duration")};
	const TCHAR* Diagnostics[] = {TEXT(""), TEXT("job ID"), TEXT("convention"), TEXT("dimensions"),
		TEXT("version"), TEXT("phase"), TEXT("incomplete response"), TEXT("payload size"), TEXT("compute time")};
	for (int32 ResponseCase = 0; ResponseCase < static_cast<int32>(UE_ARRAY_COUNT(Cases)); ++ResponseCase)
	{
		FCGHScriptedPeer Peer;
		FCGHDockerTestScene Scene;
		if (!Peer.Start(*this, ResponseCase) || !Scene.Initialize(*this, Peer.Port))
		{
			return false;
		}
		const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
		TestTrue(TEXT("A real actor request reaches the scripted TCP peer"), Scene.Solver->StartSolve());
		if (!WaitForDockerActor(*this, *Scene.Solver))
		{
			return false;
		}
		TestTrue(FString::Printf(TEXT("The peer receives the complete request before sending %s"), Cases[ResponseCase]), Peer.Worker.Get());
		if (ResponseCase == 0)
		{
			TestTrue(TEXT("The independent fragmented control response is accepted"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
			TestEqual(TEXT("The control response proves the complete socket and publication fixture works"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision + 1);
			TestEqual(TEXT("Binary64 zero phases survive fragmented reception"), Scene.SLM->GetPhasePattern().PhaseRad[0], 0.0);
		}
		else
		{
			TestTrue(FString::Printf(TEXT("The UE client rejects %s"), Cases[ResponseCase]), Scene.Solver->JobState == ECGHSolverJobState::Failed);
			TestTrue(FString::Printf(TEXT("The rejection explains %s: %s"), Cases[ResponseCase], *Scene.Solver->StatusMessage),
				Scene.Solver->StatusMessage.Contains(Diagnostics[ResponseCase]));
			TestEqual(TEXT("Malformed reception cannot overwrite the current phase revision"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
			TestEqual(TEXT("Malformed reception never publishes partial phase values"), Scene.SLM->GetPhasePattern().PhaseRad[0], 0.125);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerRoundTripTest,
	"CGH.DockerBackend.RoundTripAndBackendSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerRoundTripTest::RunTest(const FString& Parameters)
{
	int32 ExternalPort = 0;
	const bool bExternalServer = FParse::Value(FCommandLine::Get(), TEXT("CGHDockerTestPort="), ExternalPort);
	if (bExternalServer && !TestTrue(TEXT("CGHDockerTestPort is a valid TCP port"), ExternalPort > 0 && ExternalPort <= 65535))
	{
		return false;
	}
	FString Executable;
	if (!bExternalServer && !GetDockerTestExecutable(*this, Executable))
	{
		return true;
	}
	FCGHDockerTestServer Server;
	FCGHDockerTestScene Scene;
	if ((!bExternalServer && !Server.Start(*this, Executable, 20))
		|| !Scene.Initialize(*this, bExternalServer ? ExternalPort : Server.Port))
	{
		return false;
	}
	const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
	TestTrue(TEXT("The actor accepts a Docker solve"), Scene.Solver->StartSolve());
	TestEqual(TEXT("Worker reception cannot publish before game-thread polling"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	if (!WaitForDockerActor(*this, *Scene.Solver) || !CheckDummyPattern(*this, Scene))
	{
		return false;
	}
	TestEqual(TEXT("The completed Docker job publishes exactly once"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision + 1);
	const TArray<double> DummyPhases = Scene.SLM->GetPhasePattern().PhaseRad;
	Scene.Solver->Parameters.SolverBackend = ECGHSolverBackend::CPU;
	TestTrue(TEXT("The actor can switch from Docker to CPU"), Scene.Solver->StartSolve());
	if (!WaitForDockerActor(*this, *Scene.Solver))
	{
		return false;
	}
	TestTrue(TEXT("The original CPU backend still reaches Ready"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
	TestTrue(TEXT("CPU computes a physical result distinct from the dummy fixture"), Scene.SLM->GetPhasePattern().PhaseRad != DummyPhases);
	const uint64 CPURevision = Scene.SLM->GetPhasePatternRevision();
	Scene.Solver->Parameters.SolverBackend = ECGHSolverBackend::Docker;
	TestTrue(TEXT("An in-flight Docker request starts"), Scene.Solver->StartSolve());
	Scene.Solver->Parameters.SolverBackend = ECGHSolverBackend::CPU;
	Scene.Target->Parameters.InitialPhaseRad += 0.23;
	TestTrue(TEXT("A CPU request replaces in-flight Docker work"), Scene.Solver->StartSolve());
	if (!WaitForDockerActor(*this, *Scene.Solver))
	{
		return false;
	}
	TestTrue(TEXT("The replacement CPU job reaches Ready"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
	TestEqual(TEXT("Only the latest backend result advances publication"), Scene.SLM->GetPhasePatternRevision(), CPURevision + 1);
	TestTrue(TEXT("Cancelled Docker reception cannot replace the CPU result"), Scene.SLM->GetPhasePattern().PhaseRad != DummyPhases);
	Scene.Solver->Parameters.SolverBackend = ECGHSolverBackend::Docker;
	TestTrue(TEXT("The actor can switch back to Docker"), Scene.Solver->StartSolve());
	return WaitForDockerActor(*this, *Scene.Solver) && CheckDummyPattern(*this, Scene);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerCancellationTest,
	"CGH.DockerBackend.CancellationAndStalePublication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerCancellationTest::RunTest(const FString& Parameters)
{
	FString Executable;
	if (!GetDockerTestExecutable(*this, Executable))
	{
		return true;
	}
	FCGHDockerTestServer Server;
	FCGHDockerTestScene Scene;
	if (!Server.Start(*this, Executable, 250) || !Scene.Initialize(*this, Server.Port))
	{
		return false;
	}
	UCGHDockerSolverBackend* Backend = NewObject<UCGHDockerSolverBackend>();
	Backend->Settings = Scene.Solver->Parameters.Docker;
	const auto Job = Backend->Submit(Scene.CaptureInput());
	if (!TestTrue(TEXT("Cancellation test receives a job mailbox"), Job.IsValid()))
	{
		return false;
	}
	FPlatformProcess::Sleep(0.05f);
	const double CancelStarted = FPlatformTime::Seconds();
	Job->bCancelRequested.store(true, std::memory_order_relaxed);
	if (!WaitForDockerJob(*this, Job))
	{
		return false;
	}
	TestTrue(TEXT("Cancellation bounds worker completion independently of the request timeout"), FPlatformTime::Seconds() - CancelStarted < 1.0);
	TestFalse(TEXT("Cancellation cannot produce a successful result"), Job->Result.bSucceeded);
	TestTrue(TEXT("Cancellation has an explicit diagnostic"), Job->Result.Error.Contains(TEXT("cancelled")));
	TestTrue(TEXT("Cancellation never exposes a partial pattern"), Job->Result.Pattern.PhaseRad.IsEmpty());

	const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
	TestTrue(TEXT("Actor cancellation starts a remote job"), Scene.Solver->StartSolve());
	Scene.Solver->CancelSolve();
	Scene.Solver->PollSolver();
	TestTrue(TEXT("Cancelling the actor immediately enters Idle"), Scene.Solver->JobState == ECGHSolverJobState::Idle);
	TestEqual(TEXT("Cancellation retains the previous phase revision"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	for (int32 Mutation = 0; Mutation < 3; ++Mutation)
	{
		TestTrue(TEXT("A new remote job can follow cancellation or stale reception"), Scene.Solver->StartSolve());
		switch (Mutation)
		{
		case 0: Scene.Target->Parameters.InitialPhaseRad += 0.31; break;
		case 1: Scene.Solver->Parameters.Docker.RequestTimeoutSeconds += 0.5; break;
		case 2:
			if (!Scene.PublishSentinel(*this, 0.625))
			{
				return false;
			}
			break;
		}
		const uint64 GuardedRevision = Scene.SLM->GetPhasePatternRevision();
		if (!WaitForDockerActor(*this, *Scene.Solver))
		{
			return false;
		}
		TestTrue(TEXT("A response with obsolete inputs, endpoint settings, or SLM state is rejected"), Scene.Solver->JobState == ECGHSolverJobState::Failed);
		TestEqual(TEXT("Obsolete reception never overwrites the current SLM pattern"), Scene.SLM->GetPhasePatternRevision(), GuardedRevision);
		TestEqual(TEXT("The current sentinel samples are retained"), Scene.SLM->GetPhasePattern().PhaseRad[0], Mutation == 2 ? 0.625 : 0.125);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerTimeoutTest,
	"CGH.DockerBackend.TimeoutAndRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerTimeoutTest::RunTest(const FString& Parameters)
{
	FString Executable;
	if (!GetDockerTestExecutable(*this, Executable))
	{
		return true;
	}
	FCGHDockerTestServer Server;
	FCGHDockerTestScene Scene;
	if (!Server.Start(*this, Executable, 400) || !Scene.Initialize(*this, Server.Port))
	{
		return false;
	}
	Scene.Solver->Parameters.Docker.RequestTimeoutSeconds = 0.05;
	const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
	TestTrue(TEXT("A request with a short receive deadline is queued"), Scene.Solver->StartSolve());
	if (!WaitForDockerActor(*this, *Scene.Solver))
	{
		return false;
	}
	TestTrue(TEXT("An expired network request fails"), Scene.Solver->JobState == ECGHSolverJobState::Failed);
	TestTrue(TEXT("The receive timeout is actionable"), Scene.Solver->StatusMessage.Contains(TEXT("timed out")));
	TestEqual(TEXT("Timeout retains the last good publication"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	Scene.Solver->Parameters.Docker.RequestTimeoutSeconds = 3.0;
	TestTrue(TEXT("A later request can recover after timeout"), Scene.Solver->StartSolve());
	if (!WaitForDockerActor(*this, *Scene.Solver) || !CheckDummyPattern(*this, Scene))
	{
		return false;
	}
	const uint64 RecoveredRevision = Scene.SLM->GetPhasePatternRevision();
	TestTrue(TEXT("A request starts before the remote process exits"), Scene.Solver->StartSolve());
	FPlatformProcess::Sleep(0.05f);
	Server.Stop();
	if (!WaitForDockerActor(*this, *Scene.Solver))
	{
		return false;
	}
	TestTrue(TEXT("A remote disconnect fails without publishing"), Scene.Solver->JobState == ECGHSolverJobState::Failed);
	TestFalse(TEXT("A disconnect supplies a transport diagnostic"), Scene.Solver->StatusMessage.IsEmpty());
	TestEqual(TEXT("Disconnected requests preserve the recovered SLM pattern"), Scene.SLM->GetPhasePatternRevision(), RecoveredRevision);
	return true;
}

#endif
