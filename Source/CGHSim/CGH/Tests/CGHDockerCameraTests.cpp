#include "CGH/Reconstruction/CGHDockerReconstructionBackend.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "CGH/Actors/CGHCameraActor.h"
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

namespace
{
	FCGHReconstructionInput CameraInput()
	{
		FCGHReconstructionInput Input;
		Input.Mode = ECGHReconstructionMode::Camera;
		Input.SLM.ResolutionX = 4; Input.SLM.ResolutionY = 3;
		Input.SLM.PixelPitchXM = 8.e-6; Input.SLM.PixelPitchYM = 11.e-6;
		Input.SLM.ActiveWidthM = 32.e-6; Input.SLM.ActiveHeightM = 33.e-6;
		Input.Light.WavelengthM = 633.123456789e-9;
		Input.Light.Amplitude = 0.73; Input.Light.InitialPhaseRad = -0.47;
		Input.Light.DirectionSLM = FVector::XAxisVector;
		Input.Camera.OpticalPositionSLMM = FVector(0.21, 0.00013, -0.00021);
		Input.Camera.OpticalRotationSLM = FQuat(0, 0, 1, 0);
		Input.Camera.FocalLengthM = 0.015; Input.Camera.FNumber = 60.0; Input.Camera.FocusDistanceM = 0.21;
		Input.Camera.OutputResolutionX = 3; Input.Camera.OutputResolutionY = 2;
		Input.Camera.PixelPitchXM = 8.e-6; Input.Camera.PixelPitchYM = 11.e-6;
		Input.Camera.PupilResolutionX = 11; Input.Camera.PupilResolutionY = 9;
		Input.Pattern.ResolutionX = 4; Input.Pattern.ResolutionY = 3;
		for (int32 Index = 0; Index < 12; ++Index) Input.Pattern.PhaseRad.Add(0.13 + (Index % 7) * 0.07 - (Index % 11) * 0.04);
		return Input;
	}
	/** Independent CGHV 1.4 bytes expose decoder errors that a shared encoder could conceal. */
	struct FCGHCameraPeer
	{
		ISocketSubsystem* Subsystem = nullptr;
		FSocket* Listener = nullptr;
		TFuture<bool> Worker;
		std::atomic<bool> bStop{false};
		int32 Port = 0;

		~FCGHCameraPeer()
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
							&& RequestHeader[5] == 1 && RequestHeader[7] == 4 && RequestHeader[9] == 7 && RequestPayload.Num() == 256 + 12 * 8
							&& RequestPayload[3] == 4 && RequestPayload[243] == 11 && RequestPayload[247] == 9;
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
					Reply[9] = 8; // CameraReconstructionResult
					FMemory::Memcpy(Reply.GetData() + 16, RequestHeader + 16, 8);
					Reply[31] = 32 + 6 * 16;
					Reply[35] = 5; // CameraReconstructionSuccess
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
					case 5: Reply[64] = 0x7f; Reply[65] = 0xf8; break; // Quiet NaN real component.
					case 6: Reply.SetNum(72); break; // Truncated body, unchanged advertised length.
					case 7: Reply[29] = 1; break; // Payload exceeds the requested output size.
					case 8: Reply[48] = 0x7f; Reply[49] = 0xf0; break; // Infinite compute duration.
					case 9: Reply[7] = 5; break; // Unsupported future protocol minor.
					case 10: Reply[35] = 3; break; // Observer success cannot satisfy a camera request.
					case 11: Reply[9] = 6; break; // Observer result instead of camera result.
					case 12: Reply[72] = 0x7f; Reply[73] = 0xf0; break; // Infinite imaginary component.
					case 13: Reply[63] = 5; break; // Sample count mismatches dimensions.
					case 14:
						Reply.SetNumZeroed(40); Reply[9] = 4; Reply[31] = 8;
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

	bool WaitForCameraJob(FAutomationTestBase& Test, const TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe>& Job)
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

	bool CheckCameraParity(FAutomationTestBase& Test, const FCGHComplexField& Actual, const FCGHComplexField& Expected)
	{
		if (!Test.TestTrue(TEXT("CUDA returns a complete complex field"), Actual.IsValid())
			|| !Test.TestEqual(TEXT("CUDA preserves sensor width"), Actual.ResolutionX, Expected.ResolutionX)
			|| !Test.TestEqual(TEXT("CUDA preserves sensor height"), Actual.ResolutionY, Expected.ResolutionY)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerCameraTransportTest,
	"CGH.DockerReconstruction.CameraTransportValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerCameraTransportTest::RunTest(const FString& Parameters)
{
	for (int32 Case = 0; Case < 15; ++Case)
	{
		FCGHCameraPeer Peer;
		if (!Peer.Start(*this, Case)) return false;
		UCGHDockerReconstructionBackend* Backend = NewObject<UCGHDockerReconstructionBackend>();
		Backend->Settings.Port = Peer.Port;
		Backend->Settings.RequestTimeoutSeconds = 2.0;
		const auto Job = Backend->Submit(CameraInput());
		if (!WaitForCameraJob(*this, Job)) return false;
		TestTrue(TEXT("Camera request uses independent CGHV 1.4 optical payload"), Peer.Worker.Get());
		if (Case == 0)
		{
			if (!TestTrue(TEXT("Valid fragmented camera result succeeds"), Job->Result.bSucceeded)) { AddError(Job->Result.Error); return false; }
			TestEqual(TEXT("Camera sensor output width"), Job->Result.Field.ResolutionX, 3);
			TestEqual(TEXT("Camera sensor output height"), Job->Result.Field.ResolutionY, 2);
			TestEqual(TEXT("Camera field preserves complex component"), Job->Result.Field.Samples[5].Imaginary, -5.75);
		}
		else
		{
			TestFalse(FString::Printf(TEXT("Malformed camera response %d is rejected"), Case), Job->Result.bSucceeded);
			TestTrue(TEXT("Rejected camera result publishes no partial field"), Job->Result.Field.Samples.IsEmpty());
			TestFalse(TEXT("Rejected camera result explains the failure"), Job->Result.Error.IsEmpty());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHDockerCameraCudaTest,
	"CGH.DockerReconstruction.CudaCameraParityAndPublication", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHDockerCameraCudaTest::RunTest(const FString& Parameters)
{
	int32 Port = 0;
	if (!FParse::Value(FCommandLine::Get(), TEXT("CGHCudaTestPort="), Port))
	{
		AddWarning(TEXT("SKIPPED real GPU camera parity; supply -CGHCudaTestPort=<published CUDA server port> to run it."));
		return true;
	}
	if (!TestTrue(TEXT("CGHCudaTestPort is valid"), Port > 0 && Port <= 65535)) return false;
	UCGHDockerReconstructionBackend* Backend = NewObject<UCGHDockerReconstructionBackend>();
	Backend->Settings.Port = Port;
	Backend->Settings.RequestTimeoutSeconds = 10.0;
	std::atomic<bool> Cancel{false};
	for (int32 Case = 0; Case < 9; ++Case)
	{
		FCGHReconstructionInput Input = CameraInput();
		switch (Case)
		{
		case 1: Input.Light.SourceType = ECGHSourceType::PointSource; Input.Light.PositionSLMM = FVector(-0.08, 0.003, -0.005); break;
		case 2:
			Input.Camera.OpticalRotationSLM = FQuat(FVector::YAxisVector, 0.17) * FQuat(FVector::XAxisVector, 0.37) * Input.Camera.OpticalRotationSLM;
			Input.Light.DirectionSLM = FVector(0.8, 0.36, -0.48); break;
		case 3: Input.Light.Amplitude = 0.0; break;
		case 4: Input.Camera.FocalLengthM = 0.012; break;
		case 5: Input.Camera.FNumber = 85.0; break;
		case 6: Input.Camera.FocusDistanceM = 0.08; break;
		case 7: Input.Camera.PupilResolutionX = 23; Input.Camera.PupilResolutionY = 23; break;
		case 8:
			Input.Camera.OutputResolutionX = 4; Input.Camera.OutputResolutionY = 5;
			Input.Camera.PixelPitchXM = 17.e-6; Input.Camera.PupilResolutionX = 12; Input.Camera.PupilResolutionY = 8; break;
		}
		const FCGHReconstructionResult Reference = CGHReconstruction::Reconstruct(Input, Cancel);
		if (!TestTrue(TEXT("CPU camera reference succeeds"), Reference.bSucceeded)) { AddError(Reference.Error); return false; }
		const auto Job = Backend->Submit(MoveTemp(Input));
		if (!WaitForCameraJob(*this, Job)) return false;
		if (!TestTrue(FString::Printf(TEXT("Live CUDA camera case %d succeeds"), Case), Job->Result.bSucceeded)) { AddError(Job->Result.Error); return false; }
		CheckCameraParity(*this, Job->Result.Field, Reference.Field);
	}
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game)) { World.ForwardErrorMessages(this); return false; }
	ACGHSLMActor* SLM = World.GetTestWorld()->SpawnActor<ACGHSLMActor>();
	ACGHReconstructionLightActor* Light = World.GetTestWorld()->SpawnActor<ACGHReconstructionLightActor>();
	ACGHCameraActor* Camera = World.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	ACGHWorkbenchActor* Workbench = World.GetTestWorld()->SpawnActor<ACGHWorkbenchActor>();
	ACGHReconstructorActor* Actor = World.GetTestWorld()->SpawnActor<ACGHReconstructorActor>();
	if (!SLM || !Light || !Camera || !Workbench || !Actor) { AddError(TEXT("Could not create live camera actor fixture.")); return false; }
	SLM->Parameters.ResolutionX = 4; SLM->Parameters.ResolutionY = 3;
	SLM->SetActorLocationAndRotation(FVector(10.0, -5.0, 3.0), FRotator(17.0, -21.0, 6.0));
	Light->SetActorRotation(SLM->GetActorRotation());
	Camera->Parameters.OutputResolutionX = 3; Camera->Parameters.OutputResolutionY = 2;
	Camera->Parameters.FocalLengthMm = 15.0; Camera->Parameters.FNumber = 60.0; Camera->Parameters.FocusDistanceMm = 210.0;
	Camera->Parameters.SensorSampling = ECGHCameraSensorSampling::PixelPitch;
	Camera->Parameters.PixelPitchXUm = 8.0; Camera->Parameters.PixelPitchYUm = 11.0;
	Camera->Parameters.PupilResolutionX = 11; Camera->Parameters.PupilResolutionY = 9;
	Camera->SetActorLocationAndRotation(SLM->GetActorTransform().TransformPositionNoScale(FVector(21.0, .013, -.021)),
		SLM->GetActorQuat() * FQuat(0, 0, 1, 0));
	Workbench->SLM = SLM; Workbench->ReconstructionLight = Light; Workbench->Camera = Camera; Workbench->Reconstructor = Actor;
	Actor->Workbench = Workbench; Actor->Parameters.Mode = ECGHReconstructionMode::Camera;
	Actor->Parameters.ReconstructionBackend = ECGHReconstructionBackend::Docker; Actor->Parameters.Docker = Backend->Settings;
	FCGHSLMPhasePattern Pattern = CameraInput().Pattern;
	if (!TestTrue(TEXT("Live camera fixture publishes phase pattern"), SLM->SetPhasePattern(MoveTemp(Pattern)))) return false;
	FCGHReconstructionInput Input;
	FString Error;
	if (!Workbench->CaptureReconstructionInput(Input, Error, ECGHReconstructionMode::Camera)) { AddError(Error); return false; }
	Input.Pattern = SLM->GetPhasePattern();
	const FCGHReconstructionResult Reference = CGHReconstruction::Reconstruct(Input, Cancel);
	if (!Reference.bSucceeded) { AddError(Reference.Error); return false; }
	const uint64 Revision = SLM->GetPhasePatternRevision();
	if (!TestTrue(TEXT("Camera actor queues Docker reconstruction"), Actor->StartReconstruction())) { AddError(Actor->StatusMessage); return false; }
	const double Deadline = FPlatformTime::Seconds() + 12.0;
	while ((Actor->JobState == ECGHReconstructionJobState::Queued || Actor->JobState == ECGHReconstructionJobState::Running) && FPlatformTime::Seconds() < Deadline)
	{
		Actor->PollReconstructor(); FPlatformProcess::Sleep(0.001f);
	}
	if (!TestTrue(TEXT("Live camera result publishes through the camera actor"), Actor->JobState == ECGHReconstructionJobState::Ready))
	{
		Actor->CancelReconstruction(); AddError(Actor->StatusMessage); return false;
	}
	CheckCameraParity(*this, Camera->GetComplexField(), Reference.Field);
	TestEqual(TEXT("Camera reconstruction preserves source SLM phase"), SLM->GetPhasePatternRevision(), Revision);
	TestTrue(TEXT("Camera result records both-stage compute duration"), FMath::IsFinite(Actor->LastComputeSeconds) && Actor->LastComputeSeconds >= 0.0);
	return true;
}

#endif
