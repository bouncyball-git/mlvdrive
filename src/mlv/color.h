// A clip's as-shot white balance as raw channel multipliers (for the DNG's AsShotNeutral).
//
// The Kelvin -> multipliers conversion is MLV-App's kelvin_green_to_multipliers() (from UFRaw,
// src/dng/dng.c), with dcraw's cam_xyz_coeff() and the camera's D65 ColorMatrix2 from MLV-App's
// camera table.
#pragma once

#include <array>
#include <cstdint>

namespace mlv {
class Clip;
}

namespace color {

using Vec3 = std::array<double, 3>;

// The camera's as-shot white balance (WBAL), as multipliers. Defaults to 5500 K.
Vec3 asShotMultipliers(const mlv::Clip& clip);
// The as-shot Kelvin value the camera recorded (5500 if unknown or custom).
double asShotKelvin(const mlv::Clip& clip);

}  // namespace color
