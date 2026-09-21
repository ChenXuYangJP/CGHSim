#pragma once

#include "PointFocusSolver.hpp"

namespace cgh::solver::CudaPointFocus {
// FP64 port of the existing UE CGHPointFocus reference. Partitions output pixels
// across all visible CUDA devices, preserving source order and global SLM
// coordinates. Every device owns its stream and buffers; cancellation or failure
// stops and drains all workers before returning. Never falls back to a CPU or
// dummy solver. A failed/cancelled solve leaves result empty.
bool Solve(const wire::Request& request, wire::Result& result,
           std::string& error, const std::atomic<bool>& cancelled);
}
