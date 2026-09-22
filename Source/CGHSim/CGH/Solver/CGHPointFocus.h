#pragma once

#include "CGH/Types/CGHSolverTypes.h"

/** UObject-free, double-precision reference calculation; distances are meters and phases radians. */
namespace CGHPointFocus
{
	/** Aggregate limit includes every point target and every mesh sample, including zero amplitudes. */
	inline constexpr int32 MaximumEmitterCount = 1000000;

	/** Cheap validation of optical settings/target descriptions; no cloud lookup or sample traversal. */
	CGHSIM_API bool ValidateScene(const FCGHSolverInput& Input, FString& OutError);

	/** Full validation also resolves matching mesh cloud IDs/revisions and validates transformed samples. */
	CGHSIM_API bool ValidateInput(const FCGHSolverInput& Input, FString& OutError);

	/**
	 * F(P) = sum_j a_j * exp(i*(phase_j - k*distance_j)).
	 * phi_slm = wrap_[0,2*pi)(arg(F) - light_initial_phase - k*dot(DirectionSLM,P)).
	 * Pixel centers use the canonical SLM-local X=0, columns +Y, rows -Z convention.
	 * Mesh samples are transformed by target rigid pose; scale and target amplitude/phase are
	 * already baked into each sample. PointFocus uses a_j=A_j, while PointFocusInverseR uses a_j=A_j/r_j.
	 * Neither mode divides amplitudes by the sample count. PointFocus normalizes positive amplitudes
	 * by their global maximum; PointFocusInverseR uses a common per-pixel power-of-two scaling of A_j/r_j
	 * to avoid overflow/underflow without dropping amplitudes before distance weighting. Both use compensated sums.
	 * At |F| <= 32*double_epsilon*sum(scaled weights), final SLM phase is exactly zero.
	 * A single positive emitter retains the original analytical target_phase-k*r-incident phase.
	 * Plane-wave position is ignored; finite nonnegative light amplitude and finite polarization
	 * are validated but do not alter scalar phase. Direction/mesh quaternion squared-norm tolerance: 1e-6.
	 * Cancellation is checked during validation, preparation and pixel/source loops; failure clears output.
	 */
	CGHSIM_API FCGHSolverResult Solve(const FCGHSolverInput& Input,
		const std::atomic<bool>& CancelRequested);
}
