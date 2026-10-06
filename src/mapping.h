#pragma once
#include <algorithm>
#include <cmath>
// Pixel-center mapping: both end pixels map exactly to the end pixels.
inline int mapPixel(double value, int outputExtent, int sourceExtent) {
    if (outputExtent <= 1 || sourceExtent <= 1) return 0;
    return std::clamp(static_cast<int>(std::lround(
        std::clamp(value, 0.0, double(outputExtent - 1)) *
        (sourceExtent - 1) / (outputExtent - 1))), 0, sourceExtent - 1);
}
