#include "CGH/Reconstruction/CGHDockerReconstructionBackend.h"

#include "Async/Async.h"
#include "CGH/Reconstruction/CGHReconstruction.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "cgh/wire.hpp"

namespace CGHDockerReconstructionPrivate
{
namespace Wire = cgh::wire;
constexpr double SocketPollSeconds = 0.05;
constexpr int32 TransferChunkBytes = 64 * 1024;
std::atomic<uint64> NextRequestId{1};

FCGHReconstructionResult Failure(const FString& Error)
{
	FCGHReconstructionResult Result;
	Result.Error = Error;
	return Result;
}

struct FWorkerContext
{
	const FCGHReconstructionJob& Job;
	double Deadline;

	bool Check(FString& Error) const
	{
		if (Job.bCancelRequested.load(std::memory_order_acquire))
		{
			Error = TEXT("Docker reconstruction job cancelled.");
			return false;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Error = TEXT("Docker reconstruction request timed out.");
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
		Error = TEXT("Docker reconstruction requires port 1..65535 and finite positive connection/request timeouts.");
		return false;
	}
	if (Settings.Address.Len() < 7 || Settings.Address.Len() > 15)
	{
		Error = TEXT("Docker reconstruction Address must be a numeric IPv4 address (for example 127.0.0.1).");
		return false;
	}
	TArray<FString> Parts;
	Settings.Address.ParseIntoArray(Parts, TEXT("."), false);
	if (Parts.Num() != 4)
	{
		Error = TEXT("Docker reconstruction Address must be a numeric IPv4 address (for example 127.0.0.1).");
		return false;
	}
	Address = 0;
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty() || Part.Len() > 3)
		{
			Error = TEXT("Docker reconstruction Address must be a numeric IPv4 address.");
			return false;
		}
		for (const TCHAR Character : Part)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				Error = TEXT("Docker reconstruction Address must be a numeric IPv4 address.");
				return false;
			}
		}
		const uint32 Octet = static_cast<uint32>(FCString::Atoi(*Part));
		if (Octet > 255)
		{
			Error = TEXT("Docker reconstruction IPv4 address octets must be in 0..255.");
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
		Socket = Subsystem.CreateSocket(NAME_Stream, TEXT("CGH Docker reconstruction"), FNetworkProtocolTypes::IPv4);
		if (!Socket || !Socket->SetNonBlocking(true))
		{
			Error = TEXT("Docker reconstruction could not create a nonblocking TCP socket.");
			return false;
		}
		Socket->SetNoDelay(true);
		const TSharedRef<FInternetAddr> Endpoint = Subsystem.CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Endpoint->SetIp(Address);
		Endpoint->SetPort(Port);
		if (!Socket->Connect(*Endpoint) && !IsRetryable(Subsystem.GetLastErrorCode()))
		{
			Error = TEXT("Docker reconstruction TCP connection failed.");
			return false;
		}
		const double ConnectDeadline = FMath::Min(Context.Deadline, FPlatformTime::Seconds() + TimeoutSeconds);
		while (Context.Check(Error))
		{
			const double Remaining = ConnectDeadline - FPlatformTime::Seconds();
			if (Remaining <= 0.0)
			{
				Error = TEXT("Docker reconstruction TCP connection timed out.");
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
				Error = TEXT("Docker reconstruction TCP connection failed.");
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
				Error = TEXT("Docker reconstruction TCP send failed or the server closed the connection.");
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
				Error = TEXT("Docker reconstruction TCP receive failed or the server closed an incomplete response.");
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
				Error = TEXT("Docker reconstruction request timed out.");
				return false;
			}
			if (Socket->Wait(Condition, FTimespan::FromSeconds(FMath::Min(SocketPollSeconds, Remaining))))
			{
				return Context.Check(Error);
			}
			if (Socket->GetConnectionState() == SCS_ConnectionError)
			{
				Error = TEXT("Docker reconstruction TCP connection was lost.");
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
	const FCGHReconstructionInput& Input = Context.Job.Input;
	if (!CGHReconstruction::ValidateScene(Input, Error)) { return false; }
	if (Input.Mode != ECGHReconstructionMode::ObserverPlane ||
		Input.PropagationConvention != ECGHPropagationConvention::ExpPositiveIKR)
	{
		Error = TEXT("Docker protocol 1.2 requires Observer Plane reconstruction with exp(+i*k*r) propagation.");
		return false;
	}
	if (!Wire::ValidDimensions(static_cast<uint32>(Input.SLM.ResolutionX), static_cast<uint32>(Input.SLM.ResolutionY)) ||
		!Wire::ValidReconstructionDimensions(static_cast<uint32>(Input.ObserverPlane.ResolutionX), static_cast<uint32>(Input.ObserverPlane.ResolutionY)) ||
		Input.Pattern.ResolutionX != Input.SLM.ResolutionX || Input.Pattern.ResolutionY != Input.SLM.ResolutionY ||
		Input.Pattern.PhaseRad.Num() != int64(Input.SLM.ResolutionX) * Input.SLM.ResolutionY)
	{
		Error = TEXT("Docker reconstruction dimensions exceed protocol limits or the phase samples do not match the SLM grid.");
		return false;
	}
	if (!Context.Check(Error)) { return false; }
	Wire::ReconstructionRequest Request;
	Request.algorithm = Wire::Algorithm::ObserverPlaneReconstruction;
	Request.convention = Wire::Convention::ExpPositiveIKR;
	Request.slm.resolution_x = static_cast<uint32>(Input.SLM.ResolutionX);
	Request.slm.resolution_y = static_cast<uint32>(Input.SLM.ResolutionY);
	Request.slm.pixel_pitch_x_m = Input.SLM.PixelPitchXM;
	Request.slm.pixel_pitch_y_m = Input.SLM.PixelPitchYM;
	Request.slm.active_width_m = Input.SLM.ActiveWidthM;
	Request.slm.active_height_m = Input.SLM.ActiveHeightM;
	switch (Input.SLM.ModulationType)
	{
	case ECGHSLMModulationType::PhaseOnly: Request.slm.modulation = Wire::Modulation::PhaseOnly; break;
	default: Error = TEXT("Docker reconstruction requires phase-only SLM modulation."); return false;
	}
	const FCGHReconstructionLightDescription& Light = Input.Light;
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
	default: Error = TEXT("Docker reconstruction input has an unsupported light source type."); return false;
	}
	const FCGHObserverPlaneDescription& Observer = Input.ObserverPlane;
	Request.observer.resolution_x = static_cast<uint32>(Observer.ResolutionX);
	Request.observer.resolution_y = static_cast<uint32>(Observer.ResolutionY);
	Request.observer.pixel_pitch_x_m = Observer.PixelPitchXM;
	Request.observer.pixel_pitch_y_m = Observer.PixelPitchYM;
	Request.observer.position_slm_m = ToWireVector(Observer.PositionSLMM);
	Request.observer.rotation_slm = {Observer.RotationSLM.X, Observer.RotationSLM.Y, Observer.RotationSLM.Z, Observer.RotationSLM.W};
	Request.phase_radians.reserve(static_cast<size_t>(Input.Pattern.PhaseRad.Num()));
	for (int32 Index = 0; Index < Input.Pattern.PhaseRad.Num(); ++Index)
	{
		if ((Index & 1023) == 0 && !Context.Check(Error)) { return false; }
		Request.phase_radians.push_back(Input.Pattern.PhaseRad[Index]);
	}
	std::string WireError;
	if (!Wire::EncodeReconstructionRequest(Request, Payload, WireError, &Context.Job.bCancelRequested))
	{
		if (Context.Check(Error))
		{
			Error = FString::Printf(TEXT("Docker reconstruction request serialization failed: %s"), UTF8_TO_TCHAR(WireError.c_str()));
		}
		return false;
	}
	return Context.Check(Error);
}

FCGHReconstructionResult RunJob(const FCGHReconstructionJob& Job, const FCGHDockerSolverSettings& Settings,
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
		return Failure(TEXT("Docker reconstruction socket subsystem is unavailable."));
	}
	std::vector<uint8_t> RequestPayload;
	if (!EncodeInput(Context, RequestPayload, Error))
	{
		return Failure(Error);
	}
	std::string WireError;
	Wire::Header RequestHeader;
	RequestHeader.type = Wire::Type::ReconstructionRequest;
	RequestHeader.request_id = RequestId;
	RequestHeader.payload_size = RequestPayload.size();
	std::vector<uint8_t> HeaderBytes;
	if (!Wire::EncodeHeader(RequestHeader, HeaderBytes, WireError))
	{
		return Failure(FString::Printf(TEXT("Docker reconstruction invalid request header: %s"), UTF8_TO_TCHAR(WireError.c_str())));
	}
	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Subsystem)
	{
		return Failure(TEXT("Docker reconstruction socket subsystem is unavailable."));
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
		return Failure(FString::Printf(TEXT("Docker reconstruction invalid response header: %s"), UTF8_TO_TCHAR(WireError.c_str())));
	}
	if (ResponseHeader.request_id != RequestId ||
		(ResponseHeader.type != Wire::Type::ReconstructionResult && ResponseHeader.type != Wire::Type::Error))
	{
		return Failure(TEXT("Docker reconstruction response job ID or message type does not match the request."));
	}
	const uint64 PixelCount = static_cast<uint64>(Job.Input.ObserverPlane.ResolutionX) * Job.Input.ObserverPlane.ResolutionY;
	// 1.2 complex result: 32-byte metadata + two binary64 values per pixel. Reject before allocating.
	if ((ResponseHeader.type == Wire::Type::ReconstructionResult && ResponseHeader.payload_size != 32 + PixelCount * 16) ||
		(ResponseHeader.type == Wire::Type::Error &&
			(ResponseHeader.payload_size < 5 || ResponseHeader.payload_size > Wire::kMaxErrorBytes + 4)))
	{
		return Failure(TEXT("Docker reconstruction response payload size does not match the requested output."));
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
			return Failure(FString::Printf(TEXT("Docker reconstruction malformed error response: %s"), UTF8_TO_TCHAR(WireError.c_str())));
		}
		return Failure(FString::Printf(TEXT("Docker reconstruction server: %s"), UTF8_TO_TCHAR(ServerError.c_str())));
	}
	Wire::ReconstructionResult WireResult;
	if (!Wire::DecodeReconstructionResult(ResponsePayload.data(), ResponsePayload.size(), WireResult, WireError, &Job.bCancelRequested))
	{
		if (!Context.Check(Error))
		{
			return Failure(Error);
		}
		return Failure(FString::Printf(TEXT("Docker reconstruction invalid result: %s"), UTF8_TO_TCHAR(WireError.c_str())));
	}
	std::vector<uint8_t>().swap(ResponsePayload);
	if (WireResult.status != Wire::Status::ReconstructionSuccess ||
		WireResult.convention != Wire::Convention::ExpPositiveIKR ||
		WireResult.resolution_x != static_cast<uint32>(Job.Input.ObserverPlane.ResolutionX) ||
		WireResult.resolution_y != static_cast<uint32>(Job.Input.ObserverPlane.ResolutionY) ||
		WireResult.samples.size() != PixelCount)
	{
		return Failure(TEXT("Docker reconstruction result status, convention or dimensions do not match the input."));
	}
	if (!Context.Check(Error))
	{
		return Failure(Error);
	}
	FCGHReconstructionResult Result;
	Result.PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
	Result.Field.ResolutionX = static_cast<int32>(WireResult.resolution_x);
	Result.Field.ResolutionY = static_cast<int32>(WireResult.resolution_y);
	Result.Field.Samples.SetNumUninitialized(static_cast<int32>(PixelCount));
	for (int32 Index = 0; Index < Result.Field.Samples.Num(); ++Index)
	{
		if ((Index & 1023) == 0 && !Context.Check(Error))
		{
			return Failure(Error);
		}
		const Wire::ComplexSample& Sample = WireResult.samples[static_cast<size_t>(Index)];
		if (!FMath::IsFinite(Sample.real) || !FMath::IsFinite(Sample.imaginary))
		{
			return Failure(TEXT("Docker reconstruction result contains a nonfinite complex sample."));
		}
		Result.Field.Samples[Index] = FCGHComplexSample(Sample.real, Sample.imaginary);
	}
	if (!Context.Check(Error))
	{
		return Failure(Error);
	}
	if (!FMath::IsFinite(WireResult.compute_seconds) || WireResult.compute_seconds < 0.0)
	{
		return Failure(TEXT("Docker reconstruction result has an invalid compute duration."));
	}
	Result.ComputeSeconds = WireResult.compute_seconds;
	Result.bSucceeded = true;
	return Result;
}
}

TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> UCGHDockerReconstructionBackend::Submit(FCGHReconstructionInput&& Input)
{
	check(IsInGameThread());
	const TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Job =
		MakeShared<FCGHReconstructionJob, ESPMode::ThreadSafe>(MoveTemp(Input));
	const FCGHDockerSolverSettings SettingsSnapshot = Settings;
	// Initialize the module on the game thread; a worker may only use the initialized subsystem.
	const bool bSocketsAvailable = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM) != nullptr;
	uint64 RequestId = CGHDockerReconstructionPrivate::NextRequestId.fetch_add(1, std::memory_order_relaxed);
	if (RequestId == 0)
	{
		RequestId = CGHDockerReconstructionPrivate::NextRequestId.fetch_add(1, std::memory_order_relaxed);
	}
	// Dedicated threads avoid occupying the shared computation pool during network waits.
	Async(EAsyncExecution::Thread, [Job, SettingsSnapshot, RequestId, bSocketsAvailable]()
	{
		Job->bStarted.store(true, std::memory_order_release);
		FCGHReconstructionResult Result = CGHDockerReconstructionPrivate::RunJob(*Job, SettingsSnapshot, RequestId, bSocketsAvailable);
		if (Job->bCancelRequested.load(std::memory_order_acquire))
		{
			Result = CGHDockerReconstructionPrivate::Failure(TEXT("Docker reconstruction job cancelled."));
		}
		Job->Result = MoveTemp(Result);
		Job->bFinished.store(true, std::memory_order_release);
	});
	return Job;
}
