#pragma once

#include "cgh/wire.hpp"

namespace cgh::reconstruction::CudaReconstruction {
// FP64 Rayleigh-Sommerfeld I midpoint propagation matching CGHReconstruction.
// Each visible CUDA device processes a disjoint observer range, in original
// SLM sample order. Cancellation/failure drains all workers; no CPU fallback.
// A failed/cancelled call leaves the output empty.
bool Solve(const wire::ReconstructionRequest& request, wire::ReconstructionResult& result,
           std::string& error, const std::atomic<bool>& cancelled);
}
