#pragma once

#include "CGH/Types/CGHReconstructionTypes.h"

/** Double-precision scalar diffraction for observation planes and thin-lens camera sensors, independent of UObjects. */
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
	 * Camera mode uses two passes of this same propagator: SLM to a sampled circular
	 * pupil, then through exp(-i*k*(y*y+z*z)/(2*f)) to the sensor. Pupil diameter is
	 * f/FNumber; the sensor is at f/(1-f/FocusDistance) behind optical-reference -X.
	 * Camera +X looks toward the SLM, and the full optical-reference quaternion defines
	 * pupil/sensor roll. The sensor retains raw local +Y/-Z sample order (an inverted
	 * optical image). This ideal thin lens is paraxial and requires converged pupil sampling.
	 * No lens aberration, detector noise, color response, or sensor-pixel integration is modeled.
	 * Thin-lens phase and image-distance reference: https://qiweb.tudelft.nl/aoi/coherentimaging/coherentimaging/
	 * Cancellation is checked during validation, preparation and inner source loops.
	 * Failure/cancellation returns a diagnostic and an empty field.
	 */
	CGHSIM_API FCGHReconstructionResult Reconstruct(const FCGHReconstructionInput& Input,
		const std::atomic<bool>& CancelRequested);
}
