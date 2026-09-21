#pragma once

#include "CGH/Types/CGHSolverTypes.h"

/** UObject-free, double-precision reference calculation; all distances are meters and phases radians. */
namespace CGHPointFocus
{
	/** Validate one Point, a phase-only SLM, PlaneWave illumination and exp(+i*k*r) before allocation. */
	CGHSIM_API bool ValidateInput(const FCGHSolverInput& Input, FString& OutError);

	/**
	 * phi_inc(P) = light_initial_phase + k * dot(DirectionSLM, P), with P = (0, Yp, Zp).
	 * phi_slm = wrap_[0,2*pi)(target_phase - k*r - phi_inc), using centered row-major samples.
	 * PlaneWave InitialPhaseRad is defined at the SLM origin; light position is ignored.
	 * DirectionSLM must be finite and unit length: abs(length_squared - 1) <= 1e-6.
	 * Light amplitude must be finite and nonnegative (including zero), and polarization
	 * must be finite; both are validated but do not change this scalar phase pattern.
	 * Target amplitude, vector polarization and 1/r attenuation are not simulated.
	 * Cancellation is checked before allocation and once per row; failure clears all output.
	 */
	CGHSIM_API FCGHSolverResult Solve(const FCGHSolverInput& Input,
		const std::atomic<bool>& CancelRequested);
}
