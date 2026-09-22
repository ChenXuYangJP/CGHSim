#include "cgh/wire.hpp"
#include <cstdlib>
#include <iostream>

namespace w = cgh::wire;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "Failed line " << __LINE__ << ": " << #condition << '\n'; std::exit(1); } } while (false)


void PutU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) bytes[offset + byte] = static_cast<std::uint8_t>(value >> (24 - 8 * byte));
}
void PutU64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) bytes[offset + byte] = static_cast<std::uint8_t>(value >> (56 - 8 * byte));
}
void PutF64(std::vector<std::uint8_t>& bytes, std::size_t offset, double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU64(bytes, offset, bits);
}

void CheckReconstructionWire() {
    std::string error;
    std::vector<std::uint8_t> bytes;
    std::atomic<bool> cancelled{true};
    w::Header header; header.request_id = 123;
    for (const auto type : {w::Type::ReconstructionRequest, w::Type::ReconstructionResult, w::Type::CameraReconstructionRequest, w::Type::CameraReconstructionResult}) {
        header.type = type;
        CHECK(w::EncodeHeader(header, bytes, error));
        CHECK(bytes[7] == 4 && bytes[9] == static_cast<std::uint8_t>(type));
        w::Header decoded;
        CHECK(w::DecodeHeader(bytes.data(), bytes.size(), decoded, error) && decoded.type == type);
    }
    header.type = static_cast<w::Type>(9); CHECK(!w::EncodeHeader(header, bytes, error));
    CHECK(w::kMaxReconstructionPixels == 16777214);
    CHECK(w::ValidDimensions(4096, 4096));
    CHECK(!w::ValidReconstructionDimensions(4096, 4096));
    CHECK(w::ValidReconstructionDimensions(16384, 1023));
    CHECK(!w::ValidReconstructionDimensions(16385, 1));
    CHECK(!w::ValidReconstructionDimensions(0, 1));

    w::ReconstructionRequest request;
    request.slm = {2, 2, 8e-6, 9e-6, 16e-6, 18e-6, w::Modulation::PhaseOnly};
    request.light.wavelength_m = 532e-9; request.light.amplitude = 0.7;
    request.light.initial_phase_rad = -1.2; request.light.direction_slm = {1, 0, 0};
    request.light.position_slm_m = {-0.3, 0.1, 0.2}; request.light.polarization_angle_rad = 0.4;
    request.observer.resolution_x = 3; request.observer.resolution_y = 2;
    request.observer.pixel_pitch_x_m = 3e-6; request.observer.pixel_pitch_y_m = 5e-6;
    request.observer.position_slm_m = {0.2, -0.01, 0.03};
    request.observer.rotation_slm = {0.0, 0.0, 0.6, 0.8};
    // Inputs preserve arbitrary finite radians and signed zero; they are not solver output phases.
    request.phase_radians = {1.0, -0.0, -7.5, 1.25e100};
    CHECK(w::EncodeReconstructionRequest(request, bytes, error));
    CHECK(bytes.size() == 224 + 4 * 8);
    CHECK(bytes[3] == 2 && bytes[7] == 1 && bytes[11] == 2 && bytes[15] == 2);
    CHECK(bytes[139] == 3 && bytes[143] == 2 && bytes[223] == 4);
    CHECK(bytes[224] == 0x3f && bytes[225] == 0xf0 && bytes[232] == 0x80);
    const auto request_bytes = bytes;
    w::ReconstructionRequest decoded_request;
    CHECK(w::DecodeReconstructionRequest(bytes.data(), bytes.size(), decoded_request, error));
    CHECK(decoded_request.algorithm == w::Algorithm::ObserverPlaneReconstruction);
    CHECK(decoded_request.slm.active_height_m == 18e-6 && decoded_request.light.initial_phase_rad == -1.2);
    CHECK(decoded_request.light.position_slm_m.y == 0.1 && decoded_request.light.polarization_angle_rad == 0.4);
    CHECK(decoded_request.observer.resolution_x == 3 && decoded_request.observer.pixel_pitch_y_m == 5e-6);
    CHECK(decoded_request.observer.position_slm_m.z == 0.03 && decoded_request.observer.rotation_slm.w == 0.8);
    CHECK(decoded_request.phase_radians == request.phase_radians && std::signbit(decoded_request.phase_radians[1]));
    CHECK(w::EncodeReconstructionRequest(decoded_request, bytes, error) && bytes == request_bytes);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        CHECK(!w::DecodeReconstructionRequest(bytes.data(), n, decoded_request, error));
    auto damaged = bytes; damaged.push_back(0);
    CHECK(!w::DecodeReconstructionRequest(damaged.data(), damaged.size(), decoded_request, error));
    const auto reject_request_u32 = [&](std::size_t offset, std::uint32_t value) {
        auto invalid = request_bytes; PutU32(invalid, offset, value);
        CHECK(!w::DecodeReconstructionRequest(invalid.data(), invalid.size(), decoded_request, error));
    };
    reject_request_u32(0, 1); // PointFocus is not a reconstruction algorithm.
    reject_request_u32(0, 3); // InverseR is not a reconstruction algorithm either.
    reject_request_u32(4, 2);
    reject_request_u32(8, 16385);
    reject_request_u32(48, 99);
    reject_request_u32(100, 99);
    reject_request_u32(136, 16385);
    damaged = request_bytes; PutU32(damaged, 136, 4096); PutU32(damaged, 140, 4096);
    CHECK(!w::DecodeReconstructionRequest(damaged.data(), damaged.size(), decoded_request, error));
    for (std::size_t offset : {std::size_t(16), std::size_t(32), std::size_t(52), std::size_t(76),
                              std::size_t(104), std::size_t(128), std::size_t(144), std::size_t(160),
                              std::size_t(184), std::size_t(224)}) {
        damaged = request_bytes; PutF64(damaged, offset, std::numeric_limits<double>::quiet_NaN());
        CHECK(!w::DecodeReconstructionRequest(damaged.data(), damaged.size(), decoded_request, error));
    }
    damaged = request_bytes; PutF64(damaged, 60, -1);
    CHECK(!w::DecodeReconstructionRequest(damaged.data(), damaged.size(), decoded_request, error));
    damaged = request_bytes; PutF64(damaged, 144, 0);
    CHECK(!w::DecodeReconstructionRequest(damaged.data(), damaged.size(), decoded_request, error));
    for (std::uint64_t count : {std::uint64_t(0), std::uint64_t(3), std::uint64_t(w::kMaxPixels) + 1,
                                std::numeric_limits<std::uint64_t>::max()}) {
        damaged = request_bytes; PutU64(damaged, 216, count);
        CHECK(!w::DecodeReconstructionRequest(damaged.data(), damaged.size(), decoded_request, error));
    }
    CHECK(decoded_request.phase_radians == request.phase_radians); // Failed reads preserve accepted data.
    CHECK(!w::EncodeReconstructionRequest(request, bytes, error, &cancelled) && bytes.empty());
    CHECK(!w::DecodeReconstructionRequest(request_bytes.data(), request_bytes.size(), decoded_request, error, &cancelled));
    request.phase_radians.pop_back(); CHECK(!w::EncodeReconstructionRequest(request, bytes, error));
    request.phase_radians.push_back(std::numeric_limits<double>::infinity());
    CHECK(!w::EncodeReconstructionRequest(request, bytes, error));

    w::ReconstructionResult result;
    result.resolution_x = 2; result.resolution_y = 1; result.compute_seconds = 0.125;
    result.samples = {{1.0, -2.0}, {-0.0, 1e-100}};
    CHECK(w::EncodeReconstructionResult(result, bytes, error));
    CHECK(bytes.size() == 64 && bytes[3] == 3 && bytes[31] == 2);
    CHECK(bytes[32] == 0x3f && bytes[33] == 0xf0 && bytes[40] == 0xc0 && bytes[48] == 0x80);
    const auto result_bytes = bytes;
    w::ReconstructionResult decoded_result;
    CHECK(w::DecodeReconstructionResult(bytes.data(), bytes.size(), decoded_result, error));
    CHECK(decoded_result.status == w::Status::ReconstructionSuccess && decoded_result.compute_seconds == 0.125);
    CHECK(decoded_result.samples[0].real == 1.0 && decoded_result.samples[0].imaginary == -2.0);
    CHECK(std::signbit(decoded_result.samples[1].real) && decoded_result.samples[1].imaginary == 1e-100);
    CHECK(w::EncodeReconstructionResult(decoded_result, bytes, error) && bytes == result_bytes);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        CHECK(!w::DecodeReconstructionResult(bytes.data(), n, decoded_result, error));
    damaged = bytes; damaged.push_back(0);
    CHECK(!w::DecodeReconstructionResult(damaged.data(), damaged.size(), decoded_result, error));
    for (std::uint64_t count : {std::uint64_t(0), std::uint64_t(1), w::kMaxReconstructionPixels + 1,
                                std::numeric_limits<std::uint64_t>::max()}) {
        damaged = result_bytes; PutU64(damaged, 24, count);
        CHECK(!w::DecodeReconstructionResult(damaged.data(), damaged.size(), decoded_result, error));
    }
    for (std::uint32_t status : {1u, 2u, 4u, 5u}) {
        damaged = result_bytes; PutU32(damaged, 0, status);
        CHECK(!w::DecodeReconstructionResult(damaged.data(), damaged.size(), decoded_result, error));
    }
    damaged = result_bytes; PutU32(damaged, 4, 2);
    CHECK(!w::DecodeReconstructionResult(damaged.data(), damaged.size(), decoded_result, error));
    damaged = result_bytes; PutU32(damaged, 8, 4096); PutU32(damaged, 12, 4096); PutU64(damaged, 24, 4096ull * 4096);
    CHECK(!w::DecodeReconstructionResult(damaged.data(), damaged.size(), decoded_result, error));
    for (std::size_t offset : {std::size_t(16), std::size_t(32), std::size_t(40)}) {
        damaged = result_bytes; PutF64(damaged, offset, std::numeric_limits<double>::infinity());
        CHECK(!w::DecodeReconstructionResult(damaged.data(), damaged.size(), decoded_result, error));
    }
    damaged = result_bytes; PutF64(damaged, 16, -1);
    CHECK(!w::DecodeReconstructionResult(damaged.data(), damaged.size(), decoded_result, error));
    CHECK(decoded_result.samples.size() == 2 && decoded_result.samples[0].imaginary == -2.0);
    CHECK(!w::EncodeReconstructionResult(result, bytes, error, &cancelled) && bytes.empty());
    CHECK(!w::DecodeReconstructionResult(result_bytes.data(), result_bytes.size(), decoded_result, error, &cancelled));
    result.samples[0].real = std::numeric_limits<double>::quiet_NaN();
    CHECK(!w::EncodeReconstructionResult(result, bytes, error));
    result.samples[0].real = 1.0; result.samples.pop_back();
    CHECK(!w::EncodeReconstructionResult(result, bytes, error));
    w::Result old_result;
    CHECK(!w::DecodeResult(result_bytes.data(), result_bytes.size(), old_result, error));
}

void CheckCameraWire() {
    std::string error;
    std::vector<std::uint8_t> bytes;
    w::CameraReconstructionRequest request;
    request.slm = {2, 2, 8e-6, 9e-6, 16e-6, 18e-6, w::Modulation::PhaseOnly};
    request.light.wavelength_m = 532e-9; request.light.amplitude = 0.7;
    request.light.direction_slm = {1, 0, 0};
    auto& c = request.camera;
    c.optical_position_slm_m = {.2, -.01, .03}; c.optical_rotation_slm = {0, 0, 1, 0};
    c.focal_length_m = .015; c.f_number = 60; c.focus_distance_m = .2;
    c.output_resolution_x = 3; c.output_resolution_y = 2;
    c.pixel_pitch_x_m = 3e-6; c.pixel_pitch_y_m = 5e-6;
    c.pupil_resolution_x = 11; c.pupil_resolution_y = 9;
    request.phase_radians = {1, -0.0, -7.5, 1.25e100};
    CHECK(w::EncodeCameraReconstructionRequest(request, bytes, error));
    CHECK(bytes.size() == 256 + 32 && bytes[3] == 4 && bytes[219] == 3 && bytes[223] == 2);
    CHECK(bytes[243] == 11 && bytes[247] == 9 && bytes[255] == 4 && bytes[264] == 0x80);
    const auto request_bytes = bytes;
    w::CameraReconstructionRequest decoded;
    CHECK(w::DecodeCameraReconstructionRequest(bytes.data(), bytes.size(), decoded, error));
    CHECK(decoded.camera.optical_rotation_slm.z == 1 && decoded.camera.focal_length_m == .015);
    CHECK(decoded.camera.pupil_resolution_x == 11 && decoded.camera.pixel_pitch_y_m == 5e-6);
    CHECK(decoded.phase_radians == request.phase_radians && std::signbit(decoded.phase_radians[1]));
    CHECK(w::EncodeCameraReconstructionRequest(decoded, bytes, error) && bytes == request_bytes);
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!w::DecodeCameraReconstructionRequest(bytes.data(), n, decoded, error));
    auto damaged = bytes; damaged.push_back(0);
    CHECK(!w::DecodeCameraReconstructionRequest(damaged.data(), damaged.size(), decoded, error));
    for (std::size_t offset : {std::size_t(0), std::size_t(4), std::size_t(8), std::size_t(216), std::size_t(240)}) {
        damaged = request_bytes; PutU32(damaged, offset, 99999);
        CHECK(!w::DecodeCameraReconstructionRequest(damaged.data(), damaged.size(), decoded, error));
    }
    for (std::size_t offset : {std::size_t(136), std::size_t(160), std::size_t(192), std::size_t(200), std::size_t(208), std::size_t(224), std::size_t(256)}) {
        damaged = request_bytes; PutF64(damaged, offset, std::numeric_limits<double>::quiet_NaN());
        CHECK(!w::DecodeCameraReconstructionRequest(damaged.data(), damaged.size(), decoded, error));
    }
    for (std::uint64_t count : {std::uint64_t(0), std::uint64_t(3), std::numeric_limits<std::uint64_t>::max()}) {
        damaged = request_bytes; PutU64(damaged, 248, count);
        CHECK(!w::DecodeCameraReconstructionRequest(damaged.data(), damaged.size(), decoded, error));
    }
    CHECK(decoded.phase_radians == request.phase_radians);
    CHECK(w::ValidPupilDimensions(2048, 2048) && !w::ValidPupilDimensions(2049, 1) && !w::ValidPupilDimensions(1, 0));
    w::ReconstructionRequest observer;
    CHECK(!w::DecodeReconstructionRequest(request_bytes.data(), request_bytes.size(), observer, error));
    std::atomic<bool> cancelled{true};
    CHECK(!w::EncodeCameraReconstructionRequest(request, bytes, error, &cancelled) && bytes.empty());
    CHECK(!w::DecodeCameraReconstructionRequest(request_bytes.data(), request_bytes.size(), decoded, error, &cancelled));
    c.focus_distance_m = c.focal_length_m;
    CHECK(!w::EncodeCameraReconstructionRequest(request, bytes, error));

    w::CameraReconstructionResult result;
    result.resolution_x = 2; result.resolution_y = 1; result.compute_seconds = .125;
    result.samples = {{1, -2}, {-0.0, 1e-100}};
    CHECK(w::EncodeCameraReconstructionResult(result, bytes, error));
    CHECK(bytes.size() == 64 && bytes[3] == 5 && bytes[31] == 2 && bytes[48] == 0x80);
    const auto result_bytes = bytes;
    w::CameraReconstructionResult output;
    CHECK(w::DecodeCameraReconstructionResult(bytes.data(), bytes.size(), output, error));
    CHECK(output.samples[0].imaginary == -2 && std::signbit(output.samples[1].real));
    CHECK(w::EncodeCameraReconstructionResult(output, bytes, error) && bytes == result_bytes);
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!w::DecodeCameraReconstructionResult(bytes.data(), n, output, error));
    for (std::uint32_t status : {1u, 2u, 3u, 4u}) {
        damaged = result_bytes; PutU32(damaged, 0, status);
        CHECK(!w::DecodeCameraReconstructionResult(damaged.data(), damaged.size(), output, error));
    }
    damaged = result_bytes; PutU64(damaged, 24, std::numeric_limits<std::uint64_t>::max());
    CHECK(!w::DecodeCameraReconstructionResult(damaged.data(), damaged.size(), output, error));
    damaged = result_bytes; PutF64(damaged, 32, std::numeric_limits<double>::infinity());
    CHECK(!w::DecodeCameraReconstructionResult(damaged.data(), damaged.size(), output, error));
    CHECK(output.samples.size() == 2 && output.samples[0].imaginary == -2);
    CHECK(!w::EncodeCameraReconstructionResult(result, bytes, error, &cancelled) && bytes.empty());
    CHECK(!w::DecodeCameraReconstructionResult(result_bytes.data(), result_bytes.size(), output, error, &cancelled));
    w::ReconstructionResult observer_result; w::Result solver_result;
    CHECK(!w::DecodeReconstructionResult(result_bytes.data(), result_bytes.size(), observer_result, error));
    CHECK(!w::DecodeResult(result_bytes.data(), result_bytes.size(), solver_result, error));
}

int main() {
    std::string error;
    std::vector<std::uint8_t> bytes;
    w::Header h; h.request_id = 0x0102030405060708ull; h.payload_size = 0x01020304;
    CHECK(w::EncodeHeader(h, bytes, error));
    CHECK(bytes.size() == 32 && bytes[0] == 'C' && bytes[3] == 'V' && bytes[5] == 1 && bytes[7] == 4);
    CHECK(bytes[16] == 1 && bytes[23] == 8 && bytes[28] == 1 && bytes[31] == 4);
    w::Header decoded_header;
    CHECK(w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error));
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!w::DecodeHeader(bytes.data(), n, decoded_header, error));
    bytes[7] = 0; CHECK(!w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error));
    bytes[7] = 1; CHECK(!w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error));
    bytes[7] = 2; CHECK(!w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error));
    bytes[7] = 3; CHECK(!w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error));
    bytes[7] = 5; CHECK(!w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error)); bytes[7] = 4;
    bytes[11] = 1; CHECK(!w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error));
    h.payload_size = w::kMaxPayloadBytes + 1; CHECK(!w::EncodeHeader(h, bytes, error));
    h.payload_size = 1; h.type = w::Type::Cancel; CHECK(!w::EncodeHeader(h, bytes, error));

    w::Request q;
    q.slm = {7, 5, 8e-6, 9e-6, 56e-6, 45e-6, w::Modulation::PhaseOnly};
    q.light.wavelength_m = 532e-9; q.light.amplitude = 1; q.light.direction_slm.x = 1;
    w::Target t; t.resource_id = 0x1122334455667788ull; t.revision = 9; t.kind = w::TargetKind::Mesh; t.amplitude = .7; t.phase_rad = -.2; t.position_slm_m = {.1, -.02, .03}; q.targets.push_back(t);
    w::Point point; point.position_local_m = {1e-3, 2e-3, 3e-3}; point.normal_local.z = 1; point.amplitude = .2; point.phase = -.8; point.u = .25; point.v = .75;
    w::PointCloud cloud; cloud.resource_id = t.resource_id; cloud.revision = t.revision; cloud.points.push_back(point); q.point_clouds.push_back(cloud);
    CHECK(w::EncodeRequest(q, bytes, error)); CHECK(bytes.size() == 436);
    w::Request decoded;
    CHECK(w::DecodeRequest(bytes.data(), bytes.size(), decoded, error));
    CHECK(decoded.targets[0].resource_id == t.resource_id && decoded.targets[0].revision == 9);
    CHECK(decoded.point_clouds[0].points[0].phase == -.8 && decoded.point_clouds[0].points[0].v == .75);
    CHECK(decoded.slm.pixel_pitch_x_m == 8e-6 && decoded.light.wavelength_m == 532e-9);
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!w::DecodeRequest(bytes.data(), n, decoded, error));
    bytes.push_back(0); CHECK(!w::DecodeRequest(bytes.data(), bytes.size(), decoded, error)); bytes.pop_back();
    auto damaged = bytes; damaged[7] = 77; CHECK(!w::DecodeRequest(damaged.data(), damaged.size(), decoded, error));
    damaged = bytes; damaged[236] = 0x7f; CHECK(!w::DecodeRequest(damaged.data(), damaged.size(), decoded, error));
    q.point_clouds[0].revision++; CHECK(!w::EncodeRequest(q, damaged, error)); q.point_clouds[0].revision--;
    q.point_clouds[0].points[0].amplitude = std::numeric_limits<double>::quiet_NaN(); CHECK(!w::EncodeRequest(q, damaged, error)); q.point_clouds[0].points[0].amplitude = .2;
    q.slm.resolution_x = 16385; CHECK(!w::EncodeRequest(q, damaged, error)); q.slm.resolution_x = 7;
    q.algorithm = w::Algorithm::ObserverPlaneReconstruction; CHECK(!w::EncodeRequest(q, damaged, error)); q.algorithm = w::Algorithm::PointFocus;
    q.algorithm = w::Algorithm::PointFocusInverseR;
    CHECK(w::EncodeRequest(q, damaged, error));
    CHECK(damaged.size() == bytes.size() && damaged[7] == 3);
    CHECK(w::DecodeRequest(damaged.data(), damaged.size(), decoded, error));
    CHECK(decoded.algorithm == w::Algorithm::PointFocusInverseR && decoded.point_clouds[0].points[0].amplitude == .2);
    damaged[7] = 1; CHECK(damaged == bytes); // Algorithm field is the only layout change.
    q.algorithm = static_cast<w::Algorithm>(4); CHECK(!w::EncodeRequest(q, damaged, error));
    q.algorithm = w::Algorithm::PointFocus;
    std::atomic<bool> cancelled{true}; CHECK(!w::EncodeRequest(q, damaged, error, &cancelled));
    CHECK(!w::DecodeRequest(bytes.data(), bytes.size(), decoded, error, &cancelled));

    w::Result result; result.resolution_x = 2; result.resolution_y = 2; result.compute_seconds = .01; result.phase_radians = {0, .5, 3, 6};
    CHECK(w::EncodeResult(result, bytes, error)); CHECK(bytes.size() == 64);
    w::Result decoded_result; CHECK(w::DecodeResult(bytes.data(), bytes.size(), decoded_result, error));
    CHECK(decoded_result.phase_radians[3] == 6 && decoded_result.status == w::Status::DummySuccess);
    result.status = w::Status::PointFocusSuccess;
    CHECK(w::EncodeResult(result, bytes, error));
    CHECK(w::DecodeResult(bytes.data(), bytes.size(), decoded_result, error));
    CHECK(decoded_result.status == w::Status::PointFocusSuccess && decoded_result.phase_radians[3] == 6);
    result.status = w::Status::PointFocusInverseRSuccess;
    CHECK(w::EncodeResult(result, bytes, error)); CHECK(bytes[3] == 4 && bytes.size() == 64);
    CHECK(w::DecodeResult(bytes.data(), bytes.size(), decoded_result, error));
    CHECK(decoded_result.status == w::Status::PointFocusInverseRSuccess && decoded_result.phase_radians[3] == 6);
    result.status = w::Status::ReconstructionSuccess; CHECK(!w::EncodeResult(result, damaged, error));
    damaged = bytes; damaged[3] = 3; CHECK(!w::DecodeResult(damaged.data(), damaged.size(), decoded_result, error));
    result.status = w::Status::PointFocusInverseRSuccess;
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!w::DecodeResult(bytes.data(), n, decoded_result, error));
    CHECK(!w::DecodeResult(bytes.data(), bytes.size(), decoded_result, error, &cancelled));
    damaged = bytes; damaged[3] = 99; CHECK(!w::DecodeResult(damaged.data(), damaged.size(), decoded_result, error));
    damaged = bytes; damaged[31] = 3; CHECK(!w::DecodeResult(damaged.data(), damaged.size(), decoded_result, error));
    result.phase_radians[0] = w::kTwoPi; CHECK(!w::EncodeResult(result, bytes, error));
    result.phase_radians[0] = -1; CHECK(!w::EncodeResult(result, bytes, error));
    result.phase_radians[0] = std::numeric_limits<double>::infinity(); CHECK(!w::EncodeResult(result, bytes, error));
    CHECK(w::EncodeError("Protocol failure", bytes, error));
    std::string message; CHECK(w::DecodeError(bytes.data(), bytes.size(), message, error)); CHECK(message == "Protocol failure");
    bytes.push_back(0); CHECK(!w::DecodeError(bytes.data(), bytes.size(), message, error));
    CheckReconstructionWire();
    CheckCameraWire();
    std::cout << "wire codec checks passed\n";
}
