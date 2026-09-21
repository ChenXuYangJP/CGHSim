#include "cgh/wire.hpp"
#include <cstdlib>
#include <iostream>

namespace w = cgh::wire;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "Failed line " << __LINE__ << ": " << #condition << '\n'; std::exit(1); } } while (false)

int main() {
    std::string error;
    std::vector<std::uint8_t> bytes;
    w::Header h; h.request_id = 0x0102030405060708ull; h.payload_size = 0x01020304;
    CHECK(w::EncodeHeader(h, bytes, error));
    CHECK(bytes.size() == 32 && bytes[0] == 'C' && bytes[3] == 'V' && bytes[5] == 1);
    CHECK(bytes[16] == 1 && bytes[23] == 8 && bytes[28] == 1 && bytes[31] == 4);
    w::Header decoded_header;
    CHECK(w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error));
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!w::DecodeHeader(bytes.data(), n, decoded_header, error));
    bytes[7] = 1; CHECK(!w::DecodeHeader(bytes.data(), bytes.size(), decoded_header, error)); bytes[7] = 0;
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
    std::atomic<bool> cancelled{true}; CHECK(!w::EncodeRequest(q, damaged, error, &cancelled));
    CHECK(!w::DecodeRequest(bytes.data(), bytes.size(), decoded, error, &cancelled));

    w::Result result; result.resolution_x = 2; result.resolution_y = 2; result.compute_seconds = .01; result.phase_radians = {0, .5, 3, 6};
    CHECK(w::EncodeResult(result, bytes, error)); CHECK(bytes.size() == 64);
    w::Result decoded_result; CHECK(w::DecodeResult(bytes.data(), bytes.size(), decoded_result, error));
    CHECK(decoded_result.phase_radians[3] == 6 && decoded_result.status == w::Status::DummySuccess);
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
    std::cout << "wire codec checks passed\n";
}
