#include "PointFocusSolver.hpp"

#include <chrono>
#include <exception>

namespace cgh::solver::DummyPointFocus {
bool Solve(const wire::Request& request, wire::Result& result,
           std::string& error, const std::atomic<bool>& cancelled) {
    result = {};
    error.clear();
    const auto start = std::chrono::steady_clock::now();
    try {
        if (!wire::ValidateRequest(request, error, &cancelled)) return false;
        wire::Result pending;
        pending.status = wire::Status::DummySuccess;
        pending.convention = request.convention;
        pending.resolution_x = request.slm.resolution_x;
        pending.resolution_y = request.slm.resolution_y;
        pending.phase_radians.resize(std::size_t(pending.resolution_x) * pending.resolution_y);
        for (std::uint32_t y = 0; y < pending.resolution_y; ++y) {
            if (cancelled.load(std::memory_order_relaxed)) return wire::Fail(error, "Dummy job cancelled.");
            for (std::uint32_t x = 0; x < pending.resolution_x; ++x) {
                pending.phase_radians[std::size_t(y) * pending.resolution_x + x] =
                    wire::kTwoPi * ((x + 3u * y) % 256u) / 256.0;
            }
        }
        if (cancelled.load(std::memory_order_relaxed)) return wire::Fail(error, "Dummy job cancelled.");
        pending.compute_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        result = std::move(pending);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Dummy host allocation failed: ") + exception.what();
        return false;
    }
}
}
