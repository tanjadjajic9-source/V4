#pragma once
// Plain-C++ image helpers shared by the dataset, the phone web page and the TV screen.
// Everything works on 8-bit RGB (3 bytes per pixel, row after row, no padding).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace XboxOTFM {

struct Rgb8Image {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> px; // w * h * 3

    bool Valid() const { return w > 0 && h > 0 && px.size() == static_cast<size_t>(w) * h * 3; }
};

enum class FitMode : uint32_t {
    CropCenter = 0, // cut the largest centred square out of the picture (recommended)
    Squeeze = 1     // squash the whole picture into a square
};

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t previous = 0);
uint64_t Fnv1a64(const uint8_t* data, size_t size);

// Resamples the rectangle [cx, cx+cw) x [cy, cy+ch) (pixel units, may be fractional) of an RGB image to dstW x dstH.
// Shrinking averages every covered source pixel (no aliasing); enlarging interpolates. Output is float, 0..255, interleaved RGB.
// Returns false (and leaves `out` empty) for empty / absurd sizes.
bool ResampleRegion(const uint8_t* rgb, uint32_t w, uint32_t h,
                    double cx, double cy, double cw, double ch,
                    uint32_t dstW, uint32_t dstH, bool flipHorizontal, std::vector<float>& out);

Rgb8Image ResizeRgb(const uint8_t* rgb, uint32_t w, uint32_t h, uint32_t dstW, uint32_t dstH);
Rgb8Image FitToSquare(const uint8_t* rgb, uint32_t w, uint32_t h, uint32_t size, FitMode mode);
Rgb8Image UpscaleNearest(const Rgb8Image& src, uint32_t factor);
Rgb8Image MakeGrid(const std::vector<Rgb8Image>& tiles, uint32_t columns, uint32_t pad, uint8_t bgR, uint8_t bgG, uint8_t bgB);
// [3, H, W] floats in [-1, 1] (what the network works with) -> 8-bit RGB
Rgb8Image FloatChwToRgb8(const float* chw, uint32_t w, uint32_t h);

std::vector<uint8_t> EncodeBmp24(const uint8_t* rgb, uint32_t w, uint32_t h);
std::vector<uint8_t> EncodePngRgb(const uint8_t* rgb, uint32_t w, uint32_t h);

} // namespace XboxOTFM
