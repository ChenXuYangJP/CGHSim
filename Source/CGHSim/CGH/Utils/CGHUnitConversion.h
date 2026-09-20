#pragma once

// Unreal scene distances are centimeters; scene-description distances are meters.
namespace CGHUnits
{
	constexpr double CmToM(double Value) { return Value * 0.01; }
	constexpr double MmToM(double Value) { return Value * 1.0e-3; }
	constexpr double UmToM(double Value) { return Value * 1.0e-6; }
	constexpr double NmToM(double Value) { return Value * 1.0e-9; }
	constexpr double MmToCm(double Value) { return Value * 0.1; }
	constexpr double UmToMm(double Value) { return Value * 1.0e-3; }
	constexpr double MToCm(double Value) { return Value * 100.0; }
}
