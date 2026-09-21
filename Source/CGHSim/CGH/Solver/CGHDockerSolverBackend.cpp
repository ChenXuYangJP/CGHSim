#include "CGH/Solver/CGHDockerSolverBackend.h"

#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "cgh/wire.hpp"

namespace
{
namespace Wire = cgh::wire;
constexpr double SocketPollSeconds = 0.05;
constexpr int32 TransferChunkBytes = 64 * 1024;
std::atomic<uint64> NextRequestId{1};

FCGHSolverResult Failure(const FString& Error)
{
	FCGHSolverResult Result;
	Result.Error = Error;
	return Result;
}

struct FWorkerContext
{
	const FCGHSolverJob& Job;
	double Deadline;

	bool Check(FString& Error) const
	{
		if (Job.bCancelRequested.load(std::memory_order_acquire))
		{
			Error = TEXT("Docker solver job cancelled.");
			return false;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Error = TEXT("Docker solver request timed out.");
			return false;
		}
		return true;
	}
};

bool ParseSettings(const FCGHDockerSolverSettings& Settings, uint32& Address, FString& Error)
{
	if (Settings.Port < 1 || Settings.Port > 65535 ||
		!FMath::IsFinite(Settings.ConnectTimeoutSeconds) || Settings.ConnectTimeoutSeconds <= 0.0 ||
		!FMath::IsFinite(Settings.RequestTimeoutSeconds) || Settings.RequestTimeoutSeconds <= 0.0)
	{
		Error = TEXT("Docker solver requires port 1..65535 and finite positive connection/request timeouts.");
		return false;
	}
	if (Settings.Address.Len() < 7 || Settings.Address.Len() > 15)
	{
		Error = TEXT("Docker solver Address must be a numeric IPv4 address (for example 127.0.0.1).");
		return false;
	}
	TArray<FString> Parts;
	Settings.Address.ParseIntoArray(Parts, TEXT("."), false);
	if (Parts.Num() != 4)
	{
		Error = TEXT("Docker solver Address must be a numeric IPv4 address (for example 127.0.0.1).");
		return false;
	}
	Address = 0;
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty() || Part.Len() > 3)
		{
			Error = TEXT("Docker solver Address must be a numeric IPv4 address.");
			return false;
		}
		for (const TCHAR Character : Part)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				Error = TEXT("Docker solver Address must be a numeric IPv4 address.");
				return false;
			}
		}
		const uint32 Octet = static_cast<uint32>(FCString::Atoi(*Part));
		if (Octet > 255)
		{
			Error = TEXT("Docker solver IPv4 address octets must be in 0..255.");
			return false;
		}
		Address = (Address << 8) | Octet;
	}
	return true;
}

bool IsRetryable(ESocketErrors Error)
{
	return Error == SE_EWOULDBLOCK || Error == SE_EINPROGRESS || Error == SE_EALREADY || Error == SE_EINTR;
}

/** One TCP connection is owned by one worker/job, including all close paths. */
class FJobConnection
{
public:
	FJobConnection(ISocketSubsystem& InSubsystem, const FWorkerContext& InContext, uint64 InRequestId)
		: Subsystem(InSubsystem), Context(InContext), RequestId(InRequestId)
	{
	}

	~FJobConnection()
	{
		if (Socket)
		{
			// A partial request cannot be followed by a cancel frame: close is authoritative.
			// Once the complete request is sent, a cancel frame is useful but never worth waiting for.
			if (bRequestSent && Context.Job.bCancelRequested.load(std::memory_order_acquire))
			{
				std::vector<uint8_t> CancelBytes;
				std::string Error;
				Wire::Header Header;
				Header.type = Wire::Type::Cancel;
				Header.request_id = RequestId;
				Header.payload_size = 0;
				if (Wire::EncodeHeader(Header, CancelBytes, Error))
				{
					int32 Sent = 0;
					Socket->Send(CancelBytes.data(), static_cast<int32>(CancelBytes.size()), Sent);
				}
			}
			Socket->Shutdown(ESocketShutdownMode::ReadWrite);
			Socket->Close();
			Subsystem.DestroySocket(Socket);
		}
	}

	bool Connect(uint32 Address, int32 Port, double TimeoutSeconds, FString& Error)
	{
		if (!Context.Check(Error))
		{
			return false;
		}
		Socket = Subsystem.CreateSocket(NAME_Stream, TEXT("CGH Docker solver"), FNetworkProtocolTypes::IPv4);
		if (!Socket || !Socket->SetNonBlocking(true))
		{
			Error = TEXT("Docker solver could not create a nonblocking TCP socket.");
			return false;
		}
		Socket->SetNoDelay(true);
		const TSharedRef<FInternetAddr> Endpoint = Subsystem.CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Endpoint->SetIp(Address);
		Endpoint->SetPort(Port);
		if (!Socket->Connect(*Endpoint) && !IsRetryable(Subsystem.GetLastErrorCode()))
		{
			Error = TEXT("Docker solver TCP connection failed.");
			return false;
		}
		const double ConnectDeadline = FMath::Min(Context.Deadline, FPlatformTime::Seconds() + TimeoutSeconds);
		while (Context.Check(Error))
		{
			const double Remaining = ConnectDeadline - FPlatformTime::Seconds();
			if (Remaining <= 0.0)
			{
				Error = TEXT("Docker solver TCP connection timed out.");
				return false;
			}
			if (Socket->Wait(ESocketWaitConditions::WaitForWrite,
				FTimespan::FromSeconds(FMath::Min(SocketPollSeconds, Remaining))))
			{
				// Nonblocking connect failures can also become writable; the first send detects those.
				return Context.Check(Error);
			}
			if (Socket->GetConnectionState() == SCS_ConnectionError)
			{
				Error = TEXT("Docker solver TCP connection failed.");
				return false;
			}
		}
		return false;
	}

	bool Send(const uint8* Data, size_t Size, FString& Error)
	{
		size_t Offset = 0;
		while (Offset < Size)
		{
			if (!Wait(ESocketWaitConditions::WaitForWrite, Error))
			{
				return false;
			}
			const int32 Chunk = static_cast<int32>(FMath::Min<size_t>(Size - Offset, TransferChunkBytes));
			int32 Sent = 0;
			const bool bSent = Socket->Send(Data + Offset, Chunk, Sent);
			if (bSent && Sent > 0)
			{
				Offset += static_cast<size_t>(Sent);
			}
			else if (!bSent && !IsRetryable(Subsystem.GetLastErrorCode()))
			{
				Error = TEXT("Docker solver TCP send failed or the server closed the connection.");
				return false;
			}
		}
		return Context.Check(Error);
	}

	bool Receive(uint8* Data, size_t Size, FString& Error)
	{
		size_t Offset = 0;
		while (Offset < Size)
		{
			if (!Wait(ESocketWaitConditions::WaitForRead, Error))
			{
				return false;
			}
			const int32 Chunk = static_cast<int32>(FMath::Min<size_t>(Size - Offset, TransferChunkBytes));
			int32 Received = 0;
			const bool bReceived = Socket->Recv(Data + Offset, Chunk, Received, ESocketReceiveFlags::None);
			if (bReceived && Received > 0)
			{
				Offset += static_cast<size_t>(Received);
			}
			else if (!bReceived)
			{
				// UE stream Recv returns true/zero for EWOULDBLOCK, false/zero on EOF.
				Error = TEXT("Docker solver TCP receive failed or the server closed an incomplete response.");
				return false;
			}
		}
		return Context.Check(Error);
	}

	void MarkRequestSent() { bRequestSent = true; }

private:
	bool Wait(ESocketWaitConditions::Type Condition, FString& Error)
	{
		while (Context.Check(Error))
		{
			const double Remaining = Context.Deadline - FPlatformTime::Seconds();
			if (Remaining <= 0.0)
			{
				Error = TEXT("Docker solver request timed out.");
				return false;
			}
			if (Socket->Wait(Condition, FTimespan::FromSeconds(FMath::Min(SocketPollSeconds, Remaining))))
			{
				return Context.Check(Error);
			}
			if (Socket->GetConnectionState() == SCS_ConnectionError)
			{
				Error = TEXT("Docker solver TCP connection was lost.");
				return false;
			}
		}
		return false;
	}

	ISocketSubsystem& Subsystem;
	const FWorkerContext& Context;
	const uint64 RequestId;
	FSocket* Socket = nullptr;
	bool bRequestSent = false;
};

template <typename VectorType>
Wire::Vec3 ToWireVector(const VectorType& Vector)
{
	return {Vector.X, Vector.Y, Vector.Z};
}

bool EncodeInput(const FWorkerContext& Context, std::vector<uint8_t>& Payload, FString& Error)
{
	const FCGHSolverInput& Input = Context.Job.Input;
	const FCGHSceneDescription& Scene = Input.Scene;
	if (Input.Algorithm != ECGHSolverAlgorithm::PointFocus ||
		Input.PropagationConvention != ECGHPropagationConvention::ExpPositiveIKR)
	{
		Error = TEXT("Docker protocol 1.1 does not support this algorithm or propagation convention.");
		return false;
	}
	if (Scene.SchemaVersion != 2 || Scene.SLM.ResolutionX < 1 || Scene.SLM.ResolutionY < 1 ||
		!Wire::ValidDimensions(static_cast<uint32>(Scene.SLM.ResolutionX), static_cast<uint32>(Scene.SLM.ResolutionY)) ||
		Scene.Camera.OutputResolutionX < 0 || Scene.Camera.OutputResolutionY < 0 ||
		static_cast<uint32>(Scene.Targets.Num()) > Wire::kMaxEmitters ||
		static_cast<uint32>(Input.PointClouds.Num()) > Wire::kMaxEmitters)
	{
		Error = TEXT("Docker solver input schema, dimensions or resource counts exceed protocol limits.");
		return false;
	}
	if (!Context.Check(Error))
	{
		return false;
	}
	Wire::Request Request;
	Request.scene_schema_version = static_cast<uint32>(Scene.SchemaVersion);
	Request.algorithm = Wire::Algorithm::PointFocus;
	Request.convention = Wire::Convention::ExpPositiveIKR;
	Request.slm.resolution_x = static_cast<uint32>(Scene.SLM.ResolutionX);
	Request.slm.resolution_y = static_cast<uint32>(Scene.SLM.ResolutionY);
	Request.slm.pixel_pitch_x_m = Scene.SLM.PixelPitchXM;
	Request.slm.pixel_pitch_y_m = Scene.SLM.PixelPitchYM;
	Request.slm.active_width_m = Scene.SLM.ActiveWidthM;
	Request.slm.active_height_m = Scene.SLM.ActiveHeightM;
	switch (Scene.SLM.ModulationType)
	{
	case ECGHSLMModulationType::PhaseOnly: Request.slm.modulation = Wire::Modulation::PhaseOnly; break;
	case ECGHSLMModulationType::Complex: Request.slm.modulation = Wire::Modulation::Complex; break;
	default: Error = TEXT("Docker solver input has an unsupported modulation type."); return false;
	}
	const FCGHReconstructionLightDescription& Light = Scene.ReconstructionLight;
	Request.light.wavelength_m = Light.WavelengthM;
	Request.light.amplitude = Light.Amplitude;
	Request.light.initial_phase_rad = Light.InitialPhaseRad;
	Request.light.direction_slm = ToWireVector(Light.DirectionSLM);
	Request.light.position_slm_m = ToWireVector(Light.PositionSLMM);
	Request.light.polarization_angle_rad = Light.PolarizationAngleRad;
	switch (Light.SourceType)
	{
	case ECGHSourceType::PlaneWave: Request.light.source = Wire::Source::PlaneWave; break;
	case ECGHSourceType::PointSource: Request.light.source = Wire::Source::PointSource; break;
	default: Error = TEXT("Docker solver input has an unsupported light source type."); return false;
	}
	const FCGHCameraDescription& Camera = Scene.Camera;
	Request.camera.optical_position_slm_m = ToWireVector(Camera.OpticalPositionSLMM);
	Request.camera.forward_direction_slm = ToWireVector(Camera.ForwardDirectionSLM);
	Request.camera.focal_length_m = Camera.FocalLengthM;
	Request.camera.f_number = Camera.FNumber;
	Request.camera.focus_distance_m = Camera.FocusDistanceM;
	Request.camera.sensor_width_m = Camera.SensorWidthM;
	Request.camera.sensor_height_m = Camera.SensorHeightM;
	Request.camera.output_resolution_x = static_cast<uint32>(Camera.OutputResolutionX);
	Request.camera.output_resolution_y = static_cast<uint32>(Camera.OutputResolutionY);
	Request.targets.reserve(static_cast<size_t>(Scene.Targets.Num()));
	uint64 Emitters = 0;
	for (int32 Index = 0; Index < Scene.Targets.Num(); ++Index)
	{
		if ((Index & 1023) == 0 && !Context.Check(Error))
		{
			return false;
		}
		const FCGHTargetDescription& Target = Scene.Targets[Index];
		Wire::Target WireTarget;
		WireTarget.resource_id = Target.ResourceId;
		WireTarget.revision = Target.Revision;
		WireTarget.rotation_slm = {Target.RotationSLM.X, Target.RotationSLM.Y, Target.RotationSLM.Z, Target.RotationSLM.W};
		WireTarget.position_slm_m = ToWireVector(Target.PositionSLMM);
		WireTarget.amplitude = Target.Amplitude;
		WireTarget.phase_rad = Target.PhaseRad;
		switch (Target.TargetType)
		{
		case ECGHTargetType::Point: WireTarget.kind = Wire::TargetKind::Point; ++Emitters; break;
		case ECGHTargetType::Mesh: WireTarget.kind = Wire::TargetKind::Mesh; break;
		default: Error = TEXT("Docker solver input has an unsupported target type."); return false;
		}
		Request.targets.push_back(WireTarget);
	}
	Request.point_clouds.reserve(static_cast<size_t>(Input.PointClouds.Num()));
	for (const FCGHPointCloudResource& Cloud : Input.PointClouds)
	{
		if (!Context.Check(Error))
		{
			return false;
		}
		Emitters += static_cast<uint64>(Cloud.Points.Num());
		if (Emitters > Wire::kMaxEmitters)
		{
			Error = TEXT("Docker solver input exceeds the aggregate emitter limit.");
			return false;
		}
		Wire::PointCloud WireCloud;
		WireCloud.resource_id = Cloud.ResourceId;
		WireCloud.revision = Cloud.Revision;
		WireCloud.points.reserve(static_cast<size_t>(Cloud.Points.Num()));
		for (int32 Index = 0; Index < Cloud.Points.Num(); ++Index)
		{
			if ((Index & 1023) == 0 && !Context.Check(Error))
			{
				return false;
			}
			const FCGHObjectPoint& Point = Cloud.Points[Index];
			Wire::Point WirePoint;
			WirePoint.position_local_m = ToWireVector(Point.PositionLocalM);
			WirePoint.normal_local = ToWireVector(Point.NormalLocal);
			WirePoint.amplitude = Point.Amplitude;
			WirePoint.phase = Point.Phase;
			WirePoint.u = Point.UV.X;
			WirePoint.v = Point.UV.Y;
			WireCloud.points.push_back(WirePoint);
		}
		Request.point_clouds.push_back(std::move(WireCloud));
	}
	std::string WireError;
	if (!Wire::EncodeRequest(Request, Payload, WireError, &Context.Job.bCancelRequested))
	{
		if (Context.Check(Error))
		{
			Error = FString::Printf(TEXT("Docker solver request serialization failed: %s"), UTF8_TO_TCHAR(WireError.c_str()));
		}
		return false;
	}
	return Context.Check(Error);
}

FCGHSolverResult RunJob(const FCGHSolverJob& Job, const FCGHDockerSolverSettings& Settings,
	uint64 RequestId, bool bSocketsAvailable)
{
	FString Error;
	uint32 Address = 0;
	if (!ParseSettings(Settings, Address, Error))
	{
		return Failure(Error);
	}
	const FWorkerContext Context{Job, FPlatformTime::Seconds() + Settings.RequestTimeoutSeconds};
	if (!Context.Check(Error))
	{
		return Failure(Error);
	}
	if (!bSocketsAvailable)
	{
		return Failure(TEXT("Docker solver socket subsystem is unavailable."));
	}
	std::vector<uint8_t> RequestPayload;
	if (!EncodeInput(Context, RequestPayload, Error))
	{
		return Failure(Error);
	}
	std::string WireError;
	Wire::Header RequestHeader;
	RequestHeader.type = Wire::Type::Request;
	RequestHeader.request_id = RequestId;
	RequestHeader.payload_size = RequestPayload.size();
	std::vector<uint8_t> HeaderBytes;
	if (!Wire::EncodeHeader(RequestHeader, HeaderBytes, WireError))
	{
		return Failure(FString::Printf(TEXT("Docker solver invalid request header: %s"), UTF8_TO_TCHAR(WireError.c_str())));
	}
	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Subsystem)
	{
		return Failure(TEXT("Docker solver socket subsystem is unavailable."));
	}
	FJobConnection Connection(*Subsystem, Context, RequestId);
	if (!Connection.Connect(Address, Settings.Port, Settings.ConnectTimeoutSeconds, Error) ||
		!Connection.Send(HeaderBytes.data(), HeaderBytes.size(), Error) ||
		!Connection.Send(RequestPayload.data(), RequestPayload.size(), Error))
	{
		return Failure(Error);
	}
	Connection.MarkRequestSent();
	std::vector<uint8_t>().swap(RequestPayload);
	HeaderBytes.resize(Wire::kHeaderSize);
	if (!Connection.Receive(HeaderBytes.data(), HeaderBytes.size(), Error))
	{
		return Failure(Error);
	}
	Wire::Header ResponseHeader;
	if (!Wire::DecodeHeader(HeaderBytes.data(), HeaderBytes.size(), ResponseHeader, WireError))
	{
		return Failure(FString::Printf(TEXT("Docker solver invalid response header: %s"), UTF8_TO_TCHAR(WireError.c_str())));
	}
	if (ResponseHeader.request_id != RequestId ||
		(ResponseHeader.type != Wire::Type::Result && ResponseHeader.type != Wire::Type::Error))
	{
		return Failure(TEXT("Docker solver response job ID or message type does not match the request."));
	}
	const uint64 PixelCount = static_cast<uint64>(Job.Input.Scene.SLM.ResolutionX) * Job.Input.Scene.SLM.ResolutionY;
	// 1.1 result: fixed 32-byte metadata + one binary64 per pixel. Reject before allocating.
	if ((ResponseHeader.type == Wire::Type::Result && ResponseHeader.payload_size != 32 + PixelCount * 8) ||
		(ResponseHeader.type == Wire::Type::Error &&
			(ResponseHeader.payload_size < 5 || ResponseHeader.payload_size > Wire::kMaxErrorBytes + 4)))
	{
		return Failure(TEXT("Docker solver response payload size does not match the requested output."));
	}
	std::vector<uint8_t> ResponsePayload(static_cast<size_t>(ResponseHeader.payload_size));
	if (!Connection.Receive(ResponsePayload.data(), ResponsePayload.size(), Error))
	{
		return Failure(Error);
	}
	if (ResponseHeader.type == Wire::Type::Error)
	{
		std::string ServerError;
		if (!Wire::DecodeError(ResponsePayload.data(), ResponsePayload.size(), ServerError, WireError))
		{
			return Failure(FString::Printf(TEXT("Docker solver malformed error response: %s"), UTF8_TO_TCHAR(WireError.c_str())));
		}
		return Failure(FString::Printf(TEXT("Docker solver server: %s"), UTF8_TO_TCHAR(ServerError.c_str())));
	}
	Wire::Result WireResult;
	if (!Wire::DecodeResult(ResponsePayload.data(), ResponsePayload.size(), WireResult, WireError, &Job.bCancelRequested))
	{
		if (!Context.Check(Error))
		{
			return Failure(Error);
		}
		return Failure(FString::Printf(TEXT("Docker solver invalid result: %s"), UTF8_TO_TCHAR(WireError.c_str())));
	}
	std::vector<uint8_t>().swap(ResponsePayload);
	if (WireResult.convention != Wire::Convention::ExpPositiveIKR ||
		WireResult.resolution_x != static_cast<uint32>(Job.Input.Scene.SLM.ResolutionX) ||
		WireResult.resolution_y != static_cast<uint32>(Job.Input.Scene.SLM.ResolutionY) ||
		WireResult.phase_radians.size() != PixelCount)
	{
		return Failure(TEXT("Docker solver result convention or dimensions do not match the input."));
	}
	if (!Context.Check(Error))
	{
		return Failure(Error);
	}
	FCGHSolverResult Result;
	Result.PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
	Result.Pattern.ResolutionX = static_cast<int32>(WireResult.resolution_x);
	Result.Pattern.ResolutionY = static_cast<int32>(WireResult.resolution_y);
	Result.Pattern.PhaseRad.SetNumUninitialized(static_cast<int32>(PixelCount));
	for (int32 Index = 0; Index < Result.Pattern.PhaseRad.Num(); ++Index)
	{
		if ((Index & 1023) == 0 && !Context.Check(Error))
		{
			return Failure(Error);
		}
		Result.Pattern.PhaseRad[Index] = WireResult.phase_radians[static_cast<size_t>(Index)];
	}
	if (!Context.Check(Error))
	{
		return Failure(Error);
	}
	Result.ComputeSeconds = WireResult.compute_seconds;
	Result.bIsDummy = WireResult.status == Wire::Status::DummySuccess;
	Result.bSucceeded = true;
	return Result;
}
}

TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> UCGHDockerSolverBackend::Submit(FCGHSolverInput&& Input)
{
	check(IsInGameThread());
	const TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> Job =
		MakeShared<FCGHSolverJob, ESPMode::ThreadSafe>(MoveTemp(Input));
	const FCGHDockerSolverSettings SettingsSnapshot = Settings;
	// Initialize the module on the game thread; a worker may only use the initialized subsystem.
	const bool bSocketsAvailable = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM) != nullptr;
	uint64 RequestId = NextRequestId.fetch_add(1, std::memory_order_relaxed);
	if (RequestId == 0)
	{
		RequestId = NextRequestId.fetch_add(1, std::memory_order_relaxed);
	}
	// Dedicated threads avoid occupying the shared computation pool during network waits.
	Async(EAsyncExecution::Thread, [Job, SettingsSnapshot, RequestId, bSocketsAvailable]()
	{
		Job->bStarted.store(true, std::memory_order_release);
		FCGHSolverResult Result = RunJob(*Job, SettingsSnapshot, RequestId, bSocketsAvailable);
		if (Job->bCancelRequested.load(std::memory_order_acquire))
		{
			Result = Failure(TEXT("Docker solver job cancelled."));
		}
		Job->Result = MoveTemp(Result);
		Job->bFinished.store(true, std::memory_order_release);
	});
	return Job;
}

