#pragma once

// CGHV 1.2: portable owned snapshots; no Unreal, CUDA, RTTI or exception dependency.
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cgh { namespace wire {
constexpr std::uint16_t kMajor = 1, kMinor = 2;
constexpr std::size_t kHeaderSize = 32;
constexpr std::uint64_t kMaxPayloadBytes = 256ull * 1024 * 1024;
constexpr std::uint32_t kMaxAxis = 16384, kMaxPixels = 16777216, kMaxEmitters = 1000000;
constexpr std::uint64_t kMaxReconstructionPixels = (kMaxPayloadBytes - 32) / 16;
constexpr std::size_t kMaxErrorBytes = 4096;
constexpr double kTwoPi = 6.283185307179586476925286766559;
enum class Type : std::uint16_t { Request = 1, Result = 2, Cancel = 3, Error = 4,
    ReconstructionRequest = 5, ReconstructionResult = 6 };
enum class Algorithm : std::uint32_t { PointFocus = 1, ObserverPlaneReconstruction = 2 };
enum class Convention : std::uint32_t { ExpPositiveIKR = 1 };
enum class Modulation : std::uint32_t { PhaseOnly = 1, Complex = 2 };
enum class Source : std::uint32_t { PlaneWave = 1, PointSource = 2 };
enum class TargetKind : std::uint32_t { Point = 1, Mesh = 2 };
enum class Status : std::uint32_t { DummySuccess = 1, PointFocusSuccess = 2, ReconstructionSuccess = 3 };
struct Header {
    Type type = Type::Request;
    std::uint64_t request_id = 0, payload_size = 0;
    std::uint16_t major = kMajor, minor = kMinor, flags = 0;
    std::uint32_t reserved = 0;
};
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Quat { double x = 0, y = 0, z = 0, w = 1; };
struct SLM {
    std::uint32_t resolution_x = 0, resolution_y = 0;
    double pixel_pitch_x_m = 0, pixel_pitch_y_m = 0, active_width_m = 0, active_height_m = 0;
    Modulation modulation = Modulation::PhaseOnly;
};
struct Light {
    double wavelength_m = 0, amplitude = 0, initial_phase_rad = 0;
    Vec3 direction_slm;
    Source source = Source::PlaneWave;
    Vec3 position_slm_m;
    double polarization_angle_rad = 0;
};
struct Camera {
    Vec3 optical_position_slm_m, forward_direction_slm;
    double focal_length_m = 0, f_number = 0, focus_distance_m = 0, sensor_width_m = 0, sensor_height_m = 0;
    std::uint32_t output_resolution_x = 0, output_resolution_y = 0;
};
struct Target {
    std::uint64_t resource_id = 0, revision = 0;
    Quat rotation_slm;
    Vec3 position_slm_m;
    double amplitude = 0, phase_rad = 0;
    TargetKind kind = TargetKind::Point;
};
struct Point {
    Vec3 position_local_m, normal_local;
    double amplitude = 0, phase = 0, u = 0, v = 0;
};
struct PointCloud {
    std::uint64_t resource_id = 0, revision = 0;
    std::vector<Point> points;
};
struct Request {
    std::uint32_t scene_schema_version = 2;
    Algorithm algorithm = Algorithm::PointFocus;
    Convention convention = Convention::ExpPositiveIKR;
    SLM slm;
    Light light;
    Camera camera;
    std::vector<Target> targets;
    std::vector<PointCloud> point_clouds;
};
struct Result {
    Status status = Status::DummySuccess;
    Convention convention = Convention::ExpPositiveIKR;
    std::uint32_t resolution_x = 0, resolution_y = 0;
    double compute_seconds = 0;
    std::vector<double> phase_radians;
};

struct ObserverPlane {
    std::uint32_t resolution_x = 0, resolution_y = 0;
    double pixel_pitch_x_m = 0, pixel_pitch_y_m = 0;
    Vec3 position_slm_m;
    Quat rotation_slm;
};
struct ReconstructionRequest {
    Algorithm algorithm = Algorithm::ObserverPlaneReconstruction;
    Convention convention = Convention::ExpPositiveIKR;
    SLM slm;
    Light light;
    ObserverPlane observer;
    std::vector<double> phase_radians;
};
struct ComplexSample { double real = 0, imaginary = 0; };
struct ReconstructionResult {
    Status status = Status::ReconstructionSuccess;
    Convention convention = Convention::ExpPositiveIKR;
    std::uint32_t resolution_x = 0, resolution_y = 0;
    double compute_seconds = 0;
    std::vector<ComplexSample> samples;
};

inline bool IsCancelled(const std::atomic<bool>* cancelled) { return cancelled && cancelled->load(std::memory_order_relaxed); }
inline bool Fail(std::string& error, const char* message) { error = message; return false; }
inline bool IsFinite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool IsFinite(const Quat& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }
inline bool ValidDimensions(std::uint32_t x, std::uint32_t y) {
    return x > 0 && y > 0 && x <= kMaxAxis && y <= kMaxAxis && std::uint64_t(x) * y <= kMaxPixels;
}
inline bool ValidReconstructionDimensions(std::uint32_t x, std::uint32_t y) {
    return x > 0 && y > 0 && x <= kMaxAxis && y <= kMaxAxis && std::uint64_t(x) * y <= kMaxReconstructionPixels;
}
inline bool ValidateReconstructionRequest(const ReconstructionRequest& request, std::string& error,
                                        const std::atomic<bool>* cancelled = nullptr) {
    error.clear();
    if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
    if (request.algorithm != Algorithm::ObserverPlaneReconstruction || request.convention != Convention::ExpPositiveIKR)
        return Fail(error, "Unsupported reconstruction algorithm or propagation convention");
    const auto& s = request.slm;
    if (!ValidDimensions(s.resolution_x, s.resolution_y)) return Fail(error, "Invalid or oversized SLM dimensions");
    if (s.modulation != Modulation::PhaseOnly && s.modulation != Modulation::Complex) return Fail(error, "Unsupported modulation enum");
    if (!std::isfinite(s.pixel_pitch_x_m) || !std::isfinite(s.pixel_pitch_y_m) || !std::isfinite(s.active_width_m) || !std::isfinite(s.active_height_m) ||
        s.pixel_pitch_x_m <= 0 || s.pixel_pitch_y_m <= 0 || s.active_width_m <= 0 || s.active_height_m <= 0)
        return Fail(error, "Invalid SLM geometry");
    const auto& l = request.light;
    if (l.source != Source::PlaneWave && l.source != Source::PointSource) return Fail(error, "Unsupported source enum");
    if (!std::isfinite(l.wavelength_m) || l.wavelength_m <= 0 || !std::isfinite(l.amplitude) || l.amplitude < 0 || !std::isfinite(l.initial_phase_rad) ||
        !IsFinite(l.direction_slm) || !IsFinite(l.position_slm_m) || !std::isfinite(l.polarization_angle_rad)) return Fail(error, "Invalid light fields");
    const auto& o = request.observer;
    if (!ValidReconstructionDimensions(o.resolution_x, o.resolution_y)) return Fail(error, "Invalid or oversized observer dimensions");
    if (!std::isfinite(o.pixel_pitch_x_m) || !std::isfinite(o.pixel_pitch_y_m) || o.pixel_pitch_x_m <= 0 || o.pixel_pitch_y_m <= 0 ||
        !IsFinite(o.position_slm_m) || !IsFinite(o.rotation_slm)) return Fail(error, "Invalid observer geometry");
    if (request.phase_radians.size() != std::uint64_t(s.resolution_x) * s.resolution_y) return Fail(error, "SLM phase count does not match dimensions");
    for (double phase : request.phase_radians) {
        if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
        if (!std::isfinite(phase)) return Fail(error, "Nonfinite SLM phase value");
    }
    return true;
}

inline bool ValidateRequest(const Request& request, std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    error.clear();
    if (request.scene_schema_version != 2) return Fail(error, "Unsupported scene schema version");
    if (request.algorithm != Algorithm::PointFocus || request.convention != Convention::ExpPositiveIKR)
        return Fail(error, "Unsupported algorithm or propagation convention");
    const auto& s = request.slm;
    if (!ValidDimensions(s.resolution_x, s.resolution_y)) return Fail(error, "Invalid or oversized SLM dimensions");
    if (s.modulation != Modulation::PhaseOnly && s.modulation != Modulation::Complex) return Fail(error, "Unsupported modulation enum");
    if (!std::isfinite(s.pixel_pitch_x_m) || !std::isfinite(s.pixel_pitch_y_m) || !std::isfinite(s.active_width_m) || !std::isfinite(s.active_height_m) ||
        s.pixel_pitch_x_m <= 0 || s.pixel_pitch_y_m <= 0 || s.active_width_m <= 0 || s.active_height_m <= 0)
        return Fail(error, "Invalid SLM geometry");
    const auto& l = request.light;
    if (l.source != Source::PlaneWave && l.source != Source::PointSource) return Fail(error, "Unsupported source enum");
    if (!std::isfinite(l.wavelength_m) || l.wavelength_m <= 0 || !std::isfinite(l.amplitude) || l.amplitude < 0 || !std::isfinite(l.initial_phase_rad) ||
        !IsFinite(l.direction_slm) || !IsFinite(l.position_slm_m) || !std::isfinite(l.polarization_angle_rad)) return Fail(error, "Invalid light fields");
    const auto& c = request.camera;
    if (!IsFinite(c.optical_position_slm_m) || !IsFinite(c.forward_direction_slm) || !std::isfinite(c.focal_length_m) || !std::isfinite(c.f_number) ||
        !std::isfinite(c.focus_distance_m) || !std::isfinite(c.sensor_width_m) || !std::isfinite(c.sensor_height_m) ||
        c.focal_length_m < 0 || c.f_number < 0 || c.focus_distance_m < 0 || c.sensor_width_m < 0 || c.sensor_height_m < 0 ||
        c.output_resolution_x > 2147483647u || c.output_resolution_y > 2147483647u) return Fail(error, "Invalid camera fields");
    if (request.targets.empty() || request.targets.size() > kMaxEmitters || request.point_clouds.size() > kMaxEmitters) return Fail(error, "Too many target resources");
    std::unordered_set<std::uint64_t> target_ids;
    std::unordered_map<std::uint64_t, std::uint64_t> cloud_revisions;
    std::uint64_t emitters = 0;
    for (const auto& t : request.targets) {
        if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
        if (t.kind != TargetKind::Point && t.kind != TargetKind::Mesh) return Fail(error, "Unsupported target enum");
        if (t.kind == TargetKind::Mesh && (!t.resource_id || !t.revision)) return Fail(error, "Mesh identity and revision must be nonzero");
        if (!target_ids.insert(t.resource_id).second) return Fail(error, "Duplicate target resource ID");
        if (!IsFinite(t.rotation_slm) || !IsFinite(t.position_slm_m) || !std::isfinite(t.amplitude) || t.amplitude < 0 || !std::isfinite(t.phase_rad))
            return Fail(error, "Invalid target fields");
        if (t.kind == TargetKind::Point) ++emitters;
    }
    for (const auto& cloud : request.point_clouds) {
        if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
        if (!cloud.resource_id || !cloud.revision || cloud.points.empty()) return Fail(error, "Invalid or empty mesh point cloud");
        if (!cloud_revisions.emplace(cloud.resource_id, cloud.revision).second) return Fail(error, "Duplicate point cloud resource ID");
        emitters += cloud.points.size();
        if (emitters > kMaxEmitters) return Fail(error, "Too many aggregate emitters");
        for (const auto& p : cloud.points) {
            if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
            if (!IsFinite(p.position_local_m) || !IsFinite(p.normal_local) || !std::isfinite(p.amplitude) || p.amplitude < 0 || !std::isfinite(p.phase) ||
                !std::isfinite(p.u) || !std::isfinite(p.v)) return Fail(error, "Invalid point sample fields");
        }
    }
    // Identity/revision joins are validated without quadratic scans.
    std::unordered_set<std::uint64_t> matched_clouds;
    for (const auto& t : request.targets) if (t.kind == TargetKind::Mesh) {
        if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
        const auto found = cloud_revisions.find(t.resource_id);
        if (found == cloud_revisions.end() || found->second != t.revision) return Fail(error, "Missing mesh point cloud or revision mismatch");
        matched_clouds.insert(t.resource_id);
    }
    if (matched_clouds.size() != request.point_clouds.size()) return Fail(error, "Unreferenced point cloud");
    return emitters <= kMaxEmitters || Fail(error, "Too many aggregate emitters");
}

namespace detail {
struct Writer {
    std::vector<std::uint8_t>& bytes;
    void U16(std::uint16_t value) { bytes.push_back(std::uint8_t(value >> 8)); bytes.push_back(std::uint8_t(value)); }
    void U32(std::uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(std::uint8_t(value >> shift)); }
    void U64(std::uint64_t value) { for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(std::uint8_t(value >> shift)); }
    void F64(double value) { static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "IEEE 754 binary64 required"); std::uint64_t bits; std::memcpy(&bits, &value, 8); U64(bits); }
    void V3(const Vec3& v) { F64(v.x); F64(v.y); F64(v.z); }
    void Q4(const Quat& q) { F64(q.x); F64(q.y); F64(q.z); F64(q.w); }
};
struct Reader {
    const std::uint8_t* bytes;
    std::size_t size, offset = 0;
    bool good = true;
    bool Have(std::size_t n) { if (!good || n > size - offset) { good = false; return false; } return true; }
    std::uint16_t U16() { if (!Have(2)) return 0; std::uint16_t v = 0; for (int i = 0; i < 2; ++i) v = std::uint16_t((v << 8) | bytes[offset++]); return v; }
    std::uint32_t U32() { if (!Have(4)) return 0; std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | bytes[offset++]; return v; }
    std::uint64_t U64() { if (!Have(8)) return 0; std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | bytes[offset++]; return v; }
    double F64() { const std::uint64_t bits = U64(); double value; std::memcpy(&value, &bits, 8); return value; }
    Vec3 V3() { Vec3 v; v.x = F64(); v.y = F64(); v.z = F64(); return v; }
    Quat Q4() { Quat q; q.x = F64(); q.y = F64(); q.z = F64(); q.w = F64(); return q; }
    bool Count(std::uint32_t count, std::size_t min_bytes) { return count <= kMaxEmitters && Have(std::size_t(count) * min_bytes); }
    bool Done() const { return good && offset == size; }
};
}

inline bool EncodeHeader(const Header& header, std::vector<std::uint8_t>& bytes, std::string& error) {
    error.clear(); bytes.clear();
    if (header.major != kMajor || header.minor != kMinor || header.flags || header.reserved) return Fail(error, "Unsupported protocol version, flags or reserved bits");
    if (header.type < Type::Request || header.type > Type::ReconstructionResult || !header.request_id || header.payload_size > kMaxPayloadBytes ||
        (header.type == Type::Cancel && header.payload_size)) return Fail(error, "Invalid frame header");
    detail::Writer w{bytes}; w.U32(0x43474856u); w.U16(header.major); w.U16(header.minor); w.U16(static_cast<std::uint16_t>(header.type));
    w.U16(header.flags); w.U32(header.reserved); w.U64(header.request_id); w.U64(header.payload_size); return true;
}
inline bool DecodeHeader(const std::uint8_t* bytes, std::size_t size, Header& header, std::string& error) {
    error.clear();
    if (size != kHeaderSize || !bytes) return Fail(error, "Truncated frame header");
    detail::Reader r{bytes, size};
    if (r.U32() != 0x43474856u) return Fail(error, "Invalid frame magic");
    Header h; h.major = r.U16(); h.minor = r.U16(); h.type = static_cast<Type>(r.U16()); h.flags = r.U16(); h.reserved = r.U32(); h.request_id = r.U64(); h.payload_size = r.U64();
    header = h; // Preserve correlation for an error response even when version validation fails.
    std::vector<std::uint8_t> ignored;
    return EncodeHeader(h, ignored, error);
}

inline bool EncodeRequest(const Request& request, std::vector<std::uint8_t>& bytes, std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    bytes.clear(); if (!ValidateRequest(request, error, cancelled)) return false;
    detail::Writer w{bytes}; w.U32(request.scene_schema_version); w.U32(static_cast<std::uint32_t>(request.algorithm)); w.U32(static_cast<std::uint32_t>(request.convention));
    const auto& s = request.slm; w.U32(s.resolution_x); w.U32(s.resolution_y); w.F64(s.pixel_pitch_x_m); w.F64(s.pixel_pitch_y_m); w.F64(s.active_width_m); w.F64(s.active_height_m); w.U32(static_cast<std::uint32_t>(s.modulation));
    const auto& l = request.light; w.F64(l.wavelength_m); w.F64(l.amplitude); w.F64(l.initial_phase_rad); w.V3(l.direction_slm); w.U32(static_cast<std::uint32_t>(l.source)); w.V3(l.position_slm_m); w.F64(l.polarization_angle_rad);
    const auto& c = request.camera; w.V3(c.optical_position_slm_m); w.V3(c.forward_direction_slm); w.F64(c.focal_length_m); w.F64(c.f_number); w.F64(c.focus_distance_m); w.F64(c.sensor_width_m); w.F64(c.sensor_height_m); w.U32(c.output_resolution_x); w.U32(c.output_resolution_y);
    w.U32(static_cast<std::uint32_t>(request.targets.size()));
    for (const auto& t : request.targets) { if (IsCancelled(cancelled)) { bytes.clear(); return Fail(error, "Cancelled"); } w.U64(t.resource_id); w.U64(t.revision); w.Q4(t.rotation_slm); w.V3(t.position_slm_m); w.F64(t.amplitude); w.F64(t.phase_rad); w.U32(static_cast<std::uint32_t>(t.kind)); }
    w.U32(static_cast<std::uint32_t>(request.point_clouds.size()));
    for (const auto& cloud : request.point_clouds) { if (IsCancelled(cancelled)) { bytes.clear(); return Fail(error, "Cancelled"); } w.U64(cloud.resource_id); w.U64(cloud.revision); w.U32(static_cast<std::uint32_t>(cloud.points.size())); for (const auto& p : cloud.points) { if (IsCancelled(cancelled)) { bytes.clear(); return Fail(error, "Cancelled"); } w.V3(p.position_local_m); w.V3(p.normal_local); w.F64(p.amplitude); w.F64(p.phase); w.F64(p.u); w.F64(p.v); } }
    if (bytes.size() > kMaxPayloadBytes) { bytes.clear(); return Fail(error, "Request exceeds payload limit"); } return true;
}
inline bool DecodeRequest(const std::uint8_t* bytes, std::size_t size, Request& request, std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    error.clear(); if (!bytes || size > kMaxPayloadBytes) return Fail(error, "Invalid request size");
    detail::Reader r{bytes, size}; Request q;
    q.scene_schema_version = r.U32(); q.algorithm = static_cast<Algorithm>(r.U32()); q.convention = static_cast<Convention>(r.U32());
    auto& s = q.slm; s.resolution_x = r.U32(); s.resolution_y = r.U32(); s.pixel_pitch_x_m = r.F64(); s.pixel_pitch_y_m = r.F64(); s.active_width_m = r.F64(); s.active_height_m = r.F64(); s.modulation = static_cast<Modulation>(r.U32());
    auto& l = q.light; l.wavelength_m = r.F64(); l.amplitude = r.F64(); l.initial_phase_rad = r.F64(); l.direction_slm = r.V3(); l.source = static_cast<Source>(r.U32()); l.position_slm_m = r.V3(); l.polarization_angle_rad = r.F64();
    auto& c = q.camera; c.optical_position_slm_m = r.V3(); c.forward_direction_slm = r.V3(); c.focal_length_m = r.F64(); c.f_number = r.F64(); c.focus_distance_m = r.F64(); c.sensor_width_m = r.F64(); c.sensor_height_m = r.F64(); c.output_resolution_x = r.U32(); c.output_resolution_y = r.U32();
    const auto targets = r.U32(); if (!r.Count(targets, 92)) return Fail(error, "Truncated or oversized target array"); q.targets.resize(targets);
    for (auto& t : q.targets) { if (IsCancelled(cancelled)) return Fail(error, "Cancelled"); t.resource_id = r.U64(); t.revision = r.U64(); t.rotation_slm = r.Q4(); t.position_slm_m = r.V3(); t.amplitude = r.F64(); t.phase_rad = r.F64(); t.kind = static_cast<TargetKind>(r.U32()); }
    const auto clouds = r.U32(); if (!r.Count(clouds, 20)) return Fail(error, "Truncated or oversized cloud array"); q.point_clouds.resize(clouds);
    std::uint64_t total_samples = 0;
    for (auto& cloud : q.point_clouds) { if (IsCancelled(cancelled)) return Fail(error, "Cancelled"); cloud.resource_id = r.U64(); cloud.revision = r.U64(); const auto count = r.U32(); total_samples += count; if (total_samples > kMaxEmitters || !r.Count(count, 80)) return Fail(error, "Truncated or oversized sample array"); cloud.points.resize(count); for (auto& p : cloud.points) { if (IsCancelled(cancelled)) return Fail(error, "Cancelled"); p.position_local_m = r.V3(); p.normal_local = r.V3(); p.amplitude = r.F64(); p.phase = r.F64(); p.u = r.F64(); p.v = r.F64(); } }
    if (!r.Done()) return Fail(error, "Truncated request or trailing bytes");
    if (!ValidateRequest(q, error, cancelled)) return false;
    request = std::move(q); return true;
}
inline bool ValidateResult(const Result& result, std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    error.clear(); if ((result.status != Status::DummySuccess && result.status != Status::PointFocusSuccess) || result.convention != Convention::ExpPositiveIKR) return Fail(error, "Unsupported result status or convention");
    if (!ValidDimensions(result.resolution_x, result.resolution_y) || result.phase_radians.size() != std::uint64_t(result.resolution_x) * result.resolution_y) return Fail(error, "Invalid result dimensions or phase count");
    if (!std::isfinite(result.compute_seconds) || result.compute_seconds < 0) return Fail(error, "Invalid compute time");
    for (double phase : result.phase_radians) { if (IsCancelled(cancelled)) return Fail(error, "Cancelled"); if (!std::isfinite(phase) || phase < 0 || phase >= kTwoPi) return Fail(error, "Invalid phase value"); }
    return true;
}
inline bool EncodeResult(const Result& result, std::vector<std::uint8_t>& bytes, std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    bytes.clear(); if (!ValidateResult(result, error, cancelled)) return false; detail::Writer w{bytes};
    w.U32(static_cast<std::uint32_t>(result.status)); w.U32(static_cast<std::uint32_t>(result.convention)); w.U32(result.resolution_x); w.U32(result.resolution_y); w.F64(result.compute_seconds); w.U64(result.phase_radians.size());
    for (double phase : result.phase_radians) { if (IsCancelled(cancelled)) { bytes.clear(); return Fail(error, "Cancelled"); } w.F64(phase); }
    return true;
}
inline bool DecodeResult(const std::uint8_t* bytes, std::size_t size, Result& result, std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    error.clear(); if (!bytes || size > kMaxPayloadBytes) return Fail(error, "Invalid result size"); detail::Reader r{bytes, size}; Result q;
    q.status = static_cast<Status>(r.U32()); q.convention = static_cast<Convention>(r.U32()); q.resolution_x = r.U32(); q.resolution_y = r.U32(); q.compute_seconds = r.F64(); const auto count = r.U64();
    if (!ValidDimensions(q.resolution_x, q.resolution_y) || count != std::uint64_t(q.resolution_x) * q.resolution_y || count > kMaxPixels || !r.Have(static_cast<std::size_t>(count) * 8)) return Fail(error, "Truncated or oversized phase array");
    q.phase_radians.resize(static_cast<std::size_t>(count)); for (auto& phase : q.phase_radians) { if (IsCancelled(cancelled)) return Fail(error, "Cancelled"); phase = r.F64(); }
    if (!r.Done()) return Fail(error, "Truncated result or trailing bytes");
    if (!ValidateResult(q, error, cancelled)) return false;
    result = std::move(q); return true;
}
inline bool EncodeReconstructionRequest(const ReconstructionRequest& request, std::vector<std::uint8_t>& bytes,
                                        std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    bytes.clear();
    if (!ValidateReconstructionRequest(request, error, cancelled)) return false;
    bytes.reserve(224 + request.phase_radians.size() * 8);
    detail::Writer w{bytes};
    w.U32(static_cast<std::uint32_t>(request.algorithm)); w.U32(static_cast<std::uint32_t>(request.convention));
    const auto& s = request.slm;
    w.U32(s.resolution_x); w.U32(s.resolution_y); w.F64(s.pixel_pitch_x_m); w.F64(s.pixel_pitch_y_m);
    w.F64(s.active_width_m); w.F64(s.active_height_m); w.U32(static_cast<std::uint32_t>(s.modulation));
    const auto& l = request.light;
    w.F64(l.wavelength_m); w.F64(l.amplitude); w.F64(l.initial_phase_rad); w.V3(l.direction_slm);
    w.U32(static_cast<std::uint32_t>(l.source)); w.V3(l.position_slm_m); w.F64(l.polarization_angle_rad);
    const auto& o = request.observer;
    w.U32(o.resolution_x); w.U32(o.resolution_y); w.F64(o.pixel_pitch_x_m); w.F64(o.pixel_pitch_y_m);
    w.V3(o.position_slm_m); w.Q4(o.rotation_slm);
    w.U64(request.phase_radians.size());
    for (double phase : request.phase_radians) {
        if (IsCancelled(cancelled)) { bytes.clear(); return Fail(error, "Cancelled"); }
        w.F64(phase);
    }
    return true;
}
inline bool DecodeReconstructionRequest(const std::uint8_t* bytes, std::size_t size, ReconstructionRequest& request,
                                        std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    error.clear();
    if (!bytes || size < 224 || size > kMaxPayloadBytes) return Fail(error, "Invalid reconstruction request size");
    if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
    detail::Reader r{bytes, size}; ReconstructionRequest q;
    q.algorithm = static_cast<Algorithm>(r.U32()); q.convention = static_cast<Convention>(r.U32());
    auto& s = q.slm;
    s.resolution_x = r.U32(); s.resolution_y = r.U32(); s.pixel_pitch_x_m = r.F64(); s.pixel_pitch_y_m = r.F64();
    s.active_width_m = r.F64(); s.active_height_m = r.F64(); s.modulation = static_cast<Modulation>(r.U32());
    auto& l = q.light;
    l.wavelength_m = r.F64(); l.amplitude = r.F64(); l.initial_phase_rad = r.F64(); l.direction_slm = r.V3();
    l.source = static_cast<Source>(r.U32()); l.position_slm_m = r.V3(); l.polarization_angle_rad = r.F64();
    auto& o = q.observer;
    o.resolution_x = r.U32(); o.resolution_y = r.U32(); o.pixel_pitch_x_m = r.F64(); o.pixel_pitch_y_m = r.F64();
    o.position_slm_m = r.V3(); o.rotation_slm = r.Q4();
    const auto count = r.U64();
    // Check both geometry and exact remaining bytes before allocating the numerical array.
    if (!ValidDimensions(s.resolution_x, s.resolution_y) || !ValidReconstructionDimensions(o.resolution_x, o.resolution_y) ||
        count != std::uint64_t(s.resolution_x) * s.resolution_y || count > kMaxPixels ||
        !r.Have(static_cast<std::size_t>(count) * 8) || size - r.offset != count * 8)
        return Fail(error, "Truncated, oversized or mismatched reconstruction phase array");
    q.phase_radians.resize(static_cast<std::size_t>(count));
    for (auto& phase : q.phase_radians) {
        if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
        phase = r.F64();
    }
    if (!r.Done()) return Fail(error, "Truncated reconstruction request or trailing bytes");
    if (!ValidateReconstructionRequest(q, error, cancelled)) return false;
    request = std::move(q); return true;
}
inline bool ValidateReconstructionResult(const ReconstructionResult& result, std::string& error,
                                         const std::atomic<bool>* cancelled = nullptr) {
    error.clear();
    if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
    if (result.status != Status::ReconstructionSuccess || result.convention != Convention::ExpPositiveIKR)
        return Fail(error, "Unsupported reconstruction status or convention");
    if (!ValidReconstructionDimensions(result.resolution_x, result.resolution_y) ||
        result.samples.size() != std::uint64_t(result.resolution_x) * result.resolution_y)
        return Fail(error, "Invalid reconstruction result dimensions or sample count");
    if (!std::isfinite(result.compute_seconds) || result.compute_seconds < 0) return Fail(error, "Invalid compute time");
    for (const auto& sample : result.samples) {
        if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
        if (!std::isfinite(sample.real) || !std::isfinite(sample.imaginary)) return Fail(error, "Nonfinite reconstructed complex sample");
    }
    return true;
}
inline bool EncodeReconstructionResult(const ReconstructionResult& result, std::vector<std::uint8_t>& bytes,
                                       std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    bytes.clear();
    if (!ValidateReconstructionResult(result, error, cancelled)) return false;
    bytes.reserve(32 + result.samples.size() * 16);
    detail::Writer w{bytes};
    w.U32(static_cast<std::uint32_t>(result.status)); w.U32(static_cast<std::uint32_t>(result.convention));
    w.U32(result.resolution_x); w.U32(result.resolution_y); w.F64(result.compute_seconds); w.U64(result.samples.size());
    for (const auto& sample : result.samples) {
        if (IsCancelled(cancelled)) { bytes.clear(); return Fail(error, "Cancelled"); }
        w.F64(sample.real); w.F64(sample.imaginary);
    }
    return true;
}
inline bool DecodeReconstructionResult(const std::uint8_t* bytes, std::size_t size, ReconstructionResult& result,
                                       std::string& error, const std::atomic<bool>* cancelled = nullptr) {
    error.clear();
    if (!bytes || size < 32 || size > kMaxPayloadBytes) return Fail(error, "Invalid reconstruction result size");
    if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
    detail::Reader r{bytes, size}; ReconstructionResult q;
    q.status = static_cast<Status>(r.U32()); q.convention = static_cast<Convention>(r.U32());
    q.resolution_x = r.U32(); q.resolution_y = r.U32(); q.compute_seconds = r.F64(); const auto count = r.U64();
    if (!ValidReconstructionDimensions(q.resolution_x, q.resolution_y) ||
        count != std::uint64_t(q.resolution_x) * q.resolution_y || count > kMaxReconstructionPixels ||
        !r.Have(static_cast<std::size_t>(count) * 16) || size - r.offset != count * 16)
        return Fail(error, "Truncated, oversized or mismatched complex sample array");
    q.samples.resize(static_cast<std::size_t>(count));
    for (auto& sample : q.samples) {
        if (IsCancelled(cancelled)) return Fail(error, "Cancelled");
        sample.real = r.F64(); sample.imaginary = r.F64();
    }
    if (!r.Done()) return Fail(error, "Truncated reconstruction result or trailing bytes");
    if (!ValidateReconstructionResult(q, error, cancelled)) return false;
    result = std::move(q); return true;
}

inline bool EncodeError(const std::string& message, std::vector<std::uint8_t>& bytes, std::string& error) {
    error.clear(); bytes.clear(); if (message.empty() || message.size() > kMaxErrorBytes) return Fail(error, "Invalid error message length"); detail::Writer w{bytes}; w.U32(static_cast<std::uint32_t>(message.size())); bytes.insert(bytes.end(), message.begin(), message.end()); return true;
}
inline bool DecodeError(const std::uint8_t* bytes, std::size_t size, std::string& message, std::string& error) {
    error.clear(); if (!bytes || size > kMaxErrorBytes + 4) return Fail(error, "Invalid error payload size"); detail::Reader r{bytes, size}; const auto count = r.U32();
    if (!count || count > kMaxErrorBytes || !r.Have(count) || size != std::size_t(count) + 4) return Fail(error, "Malformed error payload");
    message.assign(reinterpret_cast<const char*>(bytes + r.offset), count); return true;
}
}} // namespace cgh::wire
