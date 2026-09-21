#pragma once

#include "cgh/wire.hpp"

namespace cgh::solver::DummyPointFocus {
// Transport diagnostic only. A failed/cancelled solve leaves result empty.
bool Solve(const wire::Request& request, wire::Result& result,
           std::string& error, const std::atomic<bool>& cancelled);
}
