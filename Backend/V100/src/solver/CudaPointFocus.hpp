#pragma once

#include "PointFocusSolver.hpp"

namespace cgh::solver::CudaPointFocus {
// FP64 port of the existing UE CGHPointFocus reference. Uses logical CUDA
// device 0 and never falls back to a CPU or dummy solver. A failed/cancelled
// solve leaves result empty; resources are owned only by this invocation.
bool Solve(const wire::Request& request, wire::Result& result,
           std::string& error, const std::atomic<bool>& cancelled);
}
