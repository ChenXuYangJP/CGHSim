#pragma once

#include "CGH/Types/CGHComplexField.h"

namespace CGHComplexFieldPreview
{
	/** Phase uses wrapped atan2 in [0, 2*pi); amplitude uses magnitude divided by the largest magnitude.
	 * Intensity uses squared magnitude divided by the largest squared magnitude.
	 * A zero field is black in all modes. Pixels keep the field's row-major order.
	 * Invalid input clears output and supplies a diagnostic. No gamma correction is applied. */
	CGHSIM_API bool BuildGrayscale(const FCGHComplexField& Field, ECGHObserverPreviewMode Mode,
		TArray<FColor>& OutPixels, FString& OutError);
}
