#include "mlv/color.h"

#include "camid/camera_id.h"
#include "mlv/clip.h"

#include <cmath>

namespace color {

namespace {

const double kXyzToRgb[3][3] = {  // used as [row][col] exactly as UFRaw/MLV-App do
    {3.24071, -0.969258, 0.0556352},
    {-1.53726, 1.87599, -0.203996},
    {-0.498571, 0.0415557, 1.05707},
};

const double kSrgbToXyz[3][3] = {  // xyz_rgb in dcraw: linear sRGB (D65) -> XYZ
    {0.412453, 0.357580, 0.180423},
    {0.212671, 0.715160, 0.072169},
    {0.019334, 0.119193, 0.950227},
};

void TemperatureToRgb(double t, double rgb[3]) {
    double xD;
    if (t <= 4000) xD = 0.27475e9 / (t * t * t) - 0.98598e6 / (t * t) + 1.17444e3 / t + 0.145986;
    else if (t <= 7000) xD = -4.6070e9 / (t * t * t) + 2.9678e6 / (t * t) + 0.09911e3 / t + 0.244063;
    else xD = -2.0064e9 / (t * t * t) + 1.9018e6 / (t * t) + 0.24748e3 / t + 0.237040;
    double yD = -3 * xD * xD + 2.87 * xD - 0.275;
    double X = xD / yD, Y = 1, Z = (1 - xD - yD) / yD;
    double max = 0;
    for (int c = 0; c < 3; c++) {
        rgb[c] = X * kXyzToRgb[0][c] + Y * kXyzToRgb[1][c] + Z * kXyzToRgb[2][c];
        if (rgb[c] > max) max = rgb[c];
    }
    for (int c = 0; c < 3; c++) rgb[c] /= max;
}

// dcraw's pseudoinverse: out = (in^T in)^-1 in^T, transposed (size x 3).
void Pseudoinverse(const double (*in)[3], double (*out)[3], int size) {
    double work[3][6];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 6; j++) work[i][j] = j == i + 3;
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < size; k++) work[i][j] += in[k][i] * in[k][j];
    }
    for (int i = 0; i < 3; i++) {
        double num = work[i][i];
        for (int j = 0; j < 6; j++) work[i][j] /= num;
        for (int k = 0; k < 3; k++) {
            if (k == i) continue;
            num = work[k][i];
            for (int j = 0; j < 6; j++) work[k][j] -= work[i][j] * num;
        }
    }
    for (int i = 0; i < size; i++)
        for (int j = 0; j < 3; j++) {
            out[i][j] = 0;
            for (int k = 0; k < 3; k++) out[i][j] += work[j][k + 3] * in[i][k];
        }
}

void CameraXyz(uint32_t model, double camXyz[3][3]) {
    const int32_t* m = camidGetColorMatrix2(model);
    for (int i = 0; i < 9; i++) camXyz[i / 3][i % 3] = m[i * 2 + 1] ? double(m[i * 2]) / m[i * 2 + 1] : 0.0;
}

// camera <- sRGB, rows normalised to 1; preMul = 1 / row sums (dcraw's cam_xyz_coeff).
void CameraFromSrgb(uint32_t model, double camRgb[3][3], double preMul[3]) {
    double camXyz[3][3];
    CameraXyz(model, camXyz);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            camRgb[i][j] = 0;
            for (int k = 0; k < 3; k++) camRgb[i][j] += camXyz[i][k] * kSrgbToXyz[k][j];
        }
    for (int i = 0; i < 3; i++) {
        double num = camRgb[i][0] + camRgb[i][1] + camRgb[i][2];
        for (int j = 0; j < 3; j++) camRgb[i][j] /= num;
        preMul[i] = 1 / num;
    }
}

}  // namespace

Vec3 kelvinToMultipliers(double kelvin, double tint, uint32_t cameraModel) {
    // MLV-App inverts cam_rgb into rgb_cam and then back again; for a 3x3 matrix that is cam_rgb
    // itself (dcraw's pseudoinverse(A) is A^-T, and applied twice gives A).
    double back[3][3], preMul[3];
    CameraFromSrgb(cameraModel, back, preMul);

    double rgbWb[3];
    TemperatureToRgb(kelvin, rgbWb);
    const double green = std::pow(2.0, tint / 100.0 * 0.5);  // MLV-App's "green" factor
    rgbWb[1] /= green;

    Vec3 mul;
    for (int c = 0; c < 3; c++) {
        double inv = 0;
        for (int cc = 0; cc < 3; cc++) inv += 1 / preMul[c] * back[c][cc] * rgbWb[cc];
        mul[size_t(c)] = 1 / inv;
    }
    mul[0] /= mul[1];
    mul[2] /= mul[1];
    mul[1] = 1;
    return mul;
}

double asShotKelvin(const mlv::Clip& clip) {
    switch (clip.wbal.wb_mode) {
        case mlv::kWbAuto:
        case mlv::kWbKelvin: return clip.wbal.kelvin >= 1500 && clip.wbal.kelvin <= 25000 ? clip.wbal.kelvin : 5500;
        case mlv::kWbShade: return 7000;
        case mlv::kWbCloudy: return 6000;
        case mlv::kWbTungsten: return 3200;
        case mlv::kWbFluorescent: return 4000;
        default: return 5500;  // sunny, flash, custom, none
    }
}

Vec3 asShotMultipliers(const mlv::Clip& clip) {
    const auto& w = clip.wbal;
    if (w.type[0] != 0 && w.wb_mode == mlv::kWbCustom && w.wbgain_r && w.wbgain_g && w.wbgain_b) {
        // wbgain_* are the neutral (dcraw convention, 1/multiplier), MLV-App writes them as AsShotNeutral.
        return {double(w.wbgain_g) / w.wbgain_r, 1.0, double(w.wbgain_g) / w.wbgain_b};
    }
    return kelvinToMultipliers(asShotKelvin(clip), 0, clip.idnt.cameraModel);
}

Mat3 cameraToSrgb(uint32_t cameraModel) {
    double camRgb[3][3], preMul[3];
    CameraFromSrgb(cameraModel, camRgb, preMul);
    double inverse[3][3];
    Pseudoinverse(camRgb, inverse, 3);  // inverse[cam][rgb]
    Mat3 m;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) m[size_t(r * 3 + c)] = inverse[c][r];
    return m;
}

}  // namespace color
