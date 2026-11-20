// Color data for a clip: white balance multipliers and the camera -> linear sRGB matrix.
//
// The Kelvin/tint -> multipliers conversion is MLV-App's kelvin_green_to_multipliers() (from
// UFRaw, src/dng/dng.c); the matrix follows dcraw's cam_xyz_coeff() with the camera's D65
// ColorMatrix2 from MLV-App's camera table.
#pragma once

#include <array>
#include <cstdint>

namespace mlv {
class Clip;
}

namespace color {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>;  // row-major

// Raw channel multipliers (green = 1) that make a neutral under a light of this colour
// temperature neutral. `tint` > 0 adds magenta (less green), < 0 adds green; 0 neutral.
Vec3 kelvinToMultipliers(double kelvin, double tint, uint32_t cameraModel);

// The camera's as-shot white balance (WBAL), as multipliers. Defaults to 5500 K.
Vec3 asShotMultipliers(const mlv::Clip& clip);
// The as-shot Kelvin value the camera recorded (5500 if unknown or custom).
double asShotKelvin(const mlv::Clip& clip);

// Linear camera RGB (white balanced) -> linear sRGB / Rec.709 primaries, rows summing to 1.
Mat3 cameraToSrgb(uint32_t cameraModel);

}  // namespace color
