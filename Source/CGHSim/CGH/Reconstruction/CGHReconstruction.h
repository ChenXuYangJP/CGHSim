#pragma once

#include "CGH/Types/CGHReconstructionTypes.h"

/** Double-precision scalar diffraction, independent of UObjects and rendering resources. */
namespace CGHReconstruction
{
	/** Constant-work optical/geometry validation; does not traverse or require Pattern. */
	CGHSIM_API bool ValidateScene(const FCGHReconstructionInput& Input, FString& OutError);

	/** Also requires a matching, complete phase-only SLM pattern with finite values. */
	CGHSIM_API bool ValidateInput(const FCGHReconstructionInput& Input, FString& OutError);

	/**
	 * Rayleigh-Sommerfeld I, midpoint integration of a fully filled phase-only aperture:
	 * U(Q) = sum_P dA U_inc(P) exp(i*phi_slm(P)) exp(+i*k*r)
	 *                  * Q.X/(2*pi*r*r) * (1/r - i*k), r=|Q-P|.
	 * P.X=0, columns increase +Y, rows increase -Z; observer pixels use the same
	 * centered layout in their own rigid frame. All observer pixel centers require Q.X>0.
	 * PlaneWave: U_inc=A exp(i*(phi0+k*dot(direction,P))).
	 * PointSource: U_inc=A*(1 meter/r_source) exp(i*(phi0+k*r_source)); A is the
	 * amplitude at one meter, phi0 the source phase offset. Polarization is not simulated.
	 * dA=pitchX*pitchY preserves field units; no output normalization or detector cosine
	 * is applied. Samples are complex field at pixel centers, not integrated pixel power.
	 * This O(source pixels * observer pixels) reference requires adequate aperture sampling;
	 * it does not analytically integrate each finite pixel, or model fill factor/vector optics.
	 * The near-field term is retained. Convention agrees with PointFocus's exp(+i*k*r).
	 * Reference: Makris and Psaltis, Optics Communications 284 (2011), Appendix A:
	 * https://www.epfl.ch/labs/lo/wp-content/uploads/2018/08/OC_284_1686_Mar2011.pdf
	 * Cancellation is checked during validation, preparation and inner source loops.
	 * Failure/cancellation returns a diagnostic and an empty field.
	 */
	CGHSIM_API FCGHReconstructionResult Reconstruct(const FCGHReconstructionInput& Input,
		const std::atomic<bool>& CancelRequested);
}
