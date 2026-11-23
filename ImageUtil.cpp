#include "ImageUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace XboxOTFM {

namespace {

const uint32_t* CrcTable() {
    static const uint32_t* table = [] {
        uint32_t* t = new uint32_t[256];
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[n] = c;
        }
        return t;
    }();
    return table;
}

// For every output position: which source pixels contribute and with which weights (the weights of one output sum to 1).
struct AxisFilter {
    std::vector<uint32_t> first;   // first source index per output
    std::vector<uint32_t> count;   // taps per output
    std::vector<uint32_t> offset;  // where this output's weights start in `weights`
    std::vector<float> weights;
};

void BuildAxisFilter(AxisFilter& f, uint32_t srcSize, double start, double length, uint32_t outSize, bool flip) {
    f.first.assign(outSize, 0);
    f.count.assign(outSize, 0);
    f.offset.assign(outSize, 0);
    f.weights.clear();
    const double scale = length / static_cast<double>(outSize);
    for (uint32_t o = 0; o < outSize; ++o) {
        const uint32_t oo = flip ? (outSize - 1 - o) : o;
        f.offset[o] = static_cast<uint32_t>(f.weights.size());
        if (scale > 1.0) {
            const double lo = std::max(0.0, start + static_cast<double>(oo) * scale);
            const double hi = std::min(static_cast<double>(srcSize), start + static_cast<double>(oo + 1) * scale);
            int64_t p0 = static_cast<int64_t>(std::floor(lo));
            int64_t p1 = static_cast<int64_t>(std::ceil(hi));
            p0 = std::clamp<int64_t>(p0, 0, static_cast<int64_t>(srcSize) - 1);
            p1 = std::clamp<int64_t>(p1, p0 + 1, static_cast<int64_t>(srcSize));
            double total = 0.0;
            const size_t base = f.weights.size();
            for (int64_t p = p0; p < p1; ++p) {
                const double a = std::max(lo, static_cast<double>(p));
                const double b = std::min(hi, static_cast<double>(p + 1));
                const double w = std::max(0.0, b - a);
                f.weights.push_back(static_cast<float>(w));
                total += w;
            }
            if (total <= 1e-12) {
                f.weights[base] = 1.0f;
                for (size_t i = base + 1; i < f.weights.size(); ++i) f.weights[i] = 0.0f;
                total = 1.0;
            }
            for (size_t i = base; i < f.weights.size(); ++i) f.weights[i] = static_cast<float>(f.weights[i] / total);
            f.first[o] = static_cast<uint32_t>(p0);
            f.count[o] = static_cast<uint32_t>(p1 - p0);
        } else {
            const double c = start + (static_cast<double>(oo) + 0.5) * scale - 0.5;
            int64_t p0 = static_cast<int64_t>(std::floor(c));
            const double frac = c - static_cast<double>(p0);
            const int64_t last = static_cast<int64_t>(srcSize) - 1;
            const int64_t a = std::clamp<int64_t>(p0, 0, last);
            const int64_t b = std::clamp<int64_t>(p0 + 1, 0, last);
            if (a == b) {
                f.weights.push_back(1.0f);
                f.first[o] = static_cast<uint32_t>(a);
                f.count[o] = 1;
            } else {
                f.weights.push_back(static_cast<float>(1.0 - frac));
                f.weights.push_back(static_cast<float>(frac));
                f.first[o] = static_cast<uint32_t>(a);
                f.count[o] = 2;
            }
        }
    }
}

uint8_t ToByte(float v) {
    const float c = std::min(255.0f, std::max(0.0f, v));
    return static_cast<uint8_t>(c + 0.5f);
}

void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void PutChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    PutU32(out, static_cast<uint32_t>(data.size()));
    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = Crc32(out.data() + crcStart, out.size() - crcStart);
    PutU32(out, crc);
}

} // namespace

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t previous) {
    const uint32_t* table = CrcTable();
    uint32_t c = ~previous;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool ResampleRegion(const uint8_t* rgb, uint32_t w, uint32_t h,
                    double cx, double cy, double cw, double ch,
                    uint32_t dstW, uint32_t dstH, bool flipHorizontal, std::vector<float>& out) {
    out.clear();
    if (!rgb || w == 0 || h == 0 || dstW == 0 || dstH == 0 || dstW > 8192 || dstH > 8192) return false;
    if (!(cw > 0.0) || !(ch > 0.0) || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cw) || !std::isfinite(ch)) return false;
    // The region must lie inside the picture (tiny float slack is tolerated).
    cx = std::max(0.0, cx);
    cy = std::max(0.0, cy);
    cw = std::min(cw, static_cast<double>(w) - cx);
    ch = std::min(ch, static_cast<double>(h) - cy);
    if (!(cw > 0.0) || !(ch > 0.0)) return false;

    AxisFilter fx, fy;
    BuildAxisFilter(fx, w, cx, cw, dstW, flipHorizontal);
    BuildAxisFilter(fy, h, cy, ch, dstH, false);

    // Only the source rows that are used are filtered horizontally.
    uint32_t rowLo = h, rowHi = 0;
    for (uint32_t o = 0; o < dstH; ++o) {
        rowLo = std::min(rowLo, fy.first[o]);
        rowHi = std::max(rowHi, fy.first[o] + fy.count[o]);
    }
    if (rowHi <= rowLo) return false;
    const uint32_t rows = rowHi - rowLo;

    std::vector<float> tmp(static_cast<size_t>(rows) * dstW * 3);
    for (uint32_t r = 0; r < rows; ++r) {
        const uint8_t* src = rgb + static_cast<size_t>(rowLo + r) * w * 3;
        float* dst = tmp.data() + static_cast<size_t>(r) * dstW * 3;
        for (uint32_t o = 0; o < dstW; ++o) {
            float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f;
            const float* wt = fx.weights.data() + fx.offset[o];
            const uint8_t* p = src + static_cast<size_t>(fx.first[o]) * 3;
            for (uint32_t k = 0; k < fx.count[o]; ++k, p += 3) {
                a0 += wt[k] * p[0];
                a1 += wt[k] * p[1];
                a2 += wt[k] * p[2];
            }
            dst[o * 3 + 0] = a0;
            dst[o * 3 + 1] = a1;
            dst[o * 3 + 2] = a2;
        }
    }

    out.resize(static_cast<size_t>(dstW) * dstH * 3);
    for (uint32_t o = 0; o < dstH; ++o) {
        const float* wt = fy.weights.data() + fy.offset[o];
        float* dst = out.data() + static_cast<size_t>(o) * dstW * 3;
        const uint32_t firstRow = fy.first[o] - rowLo;
        for (uint32_t x = 0; x < dstW * 3; ++x) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < fy.count[o]; ++k) {
                acc += wt[k] * tmp[static_cast<size_t>(firstRow + k) * dstW * 3 + x];
            }
            dst[x] = acc;
        }
    }
    return true;
}

Rgb8Image ResizeRgb(const uint8_t* rgb, uint32_t w, uint32_t h, uint32_t dstW, uint32_t dstH) {
    Rgb8Image img;
    std::vector<float> f;
    if (!ResampleRegion(rgb, w, h, 0.0, 0.0, w, h, dstW, dstH, false, f)) return img;
    img.w = dstW;
    img.h = dstH;
    img.px.resize(f.size());
    for (size_t i = 0; i < f.size(); ++i) img.px[i] = ToByte(f[i]);
    return img;
}

Rgb8Image FitToSquare(const uint8_t* rgb, uint32_t w, uint32_t h, uint32_t size, FitMode mode) {
    Rgb8Image img;
    if (!rgb || w == 0 || h == 0 || size == 0) return img;
    double cx = 0.0, cy = 0.0, cw = w, ch = h;
    if (mode == FitMode::CropCenter) {
        const double side = static_cast<double>(std::min(w, h));
        cx = (static_cast<double>(w) - side) * 0.5;
        cy = (static_cast<double>(h) - side) * 0.5;
        cw = side;
        ch = side;
    }
    std::vector<float> f;
    if (!ResampleRegion(rgb, w, h, cx, cy, cw, ch, size, size, false, f)) return img;
    img.w = size;
    img.h = size;
    img.px.resize(f.size());
    for (size_t i = 0; i < f.size(); ++i) img.px[i] = ToByte(f[i]);
    return img;
}

Rgb8Image UpscaleNearest(const Rgb8Image& src, uint32_t factor) {
    Rgb8Image out;
    if (!src.Valid() || factor == 0 || factor > 64 || static_cast<uint64_t>(src.w) * factor > 8192 || static_cast<uint64_t>(src.h) * factor > 8192) return out;
    out.w = src.w * factor;
    out.h = src.h * factor;
    out.px.resize(static_cast<size_t>(out.w) * out.h * 3);
    for (uint32_t y = 0; y < out.h; ++y) {
        const uint8_t* srow = src.px.data() + static_cast<size_t>(y / factor) * src.w * 3;
        uint8_t* drow = out.px.data() + static_cast<size_t>(y) * out.w * 3;
        for (uint32_t x = 0; x < out.w; ++x) {
            std::memcpy(drow + static_cast<size_t>(x) * 3, srow + static_cast<size_t>(x / factor) * 3, 3);
        }
    }
    return out;
}

Rgb8Image MakeGrid(const std::vector<Rgb8Image>& tiles, uint32_t columns, uint32_t pad, uint8_t bgR, uint8_t bgG, uint8_t bgB) {
    Rgb8Image out;
    if (tiles.empty() || columns == 0) return out;
    uint32_t tw = 0, th = 0;
    for (const auto& t : tiles) {
        if (!t.Valid()) continue;
        tw = std::max(tw, t.w);
        th = std::max(th, t.h);
    }
    if (tw == 0 || th == 0) return out;
    const uint32_t cols = std::min<uint32_t>(columns, static_cast<uint32_t>(tiles.size()));
    const uint32_t rows = (static_cast<uint32_t>(tiles.size()) + cols - 1) / cols;
    const uint64_t W = static_cast<uint64_t>(cols) * tw + static_cast<uint64_t>(cols + 1) * pad;
    const uint64_t H = static_cast<uint64_t>(rows) * th + static_cast<uint64_t>(rows + 1) * pad;
    if (W > 8192 || H > 8192) return out;
    out.w = static_cast<uint32_t>(W);
    out.h = static_cast<uint32_t>(H);
    out.px.resize(static_cast<size_t>(out.w) * out.h * 3);
    for (size_t i = 0; i < out.px.size(); i += 3) {
        out.px[i] = bgR;
        out.px[i + 1] = bgG;
        out.px[i + 2] = bgB;
    }
    for (size_t i = 0; i < tiles.size(); ++i) {
        const Rgb8Image& t = tiles[i];
        if (!t.Valid()) continue;
        const uint32_t col = static_cast<uint32_t>(i) % cols;
        const uint32_t row = static_cast<uint32_t>(i) / cols;
        const uint32_t x0 = pad + col * (tw + pad) + (tw - t.w) / 2;
        const uint32_t y0 = pad + row * (th + pad) + (th - t.h) / 2;
        for (uint32_t y = 0; y < t.h; ++y) {
            std::memcpy(out.px.data() + (static_cast<size_t>(y0 + y) * out.w + x0) * 3,
                        t.px.data() + static_cast<size_t>(y) * t.w * 3, static_cast<size_t>(t.w) * 3);
        }
    }
    return out;
}

Rgb8Image FloatChwToRgb8(const float* chw, uint32_t w, uint32_t h) {
    Rgb8Image img;
    if (!chw || w == 0 || h == 0) return img;
    img.w = w;
    img.h = h;
    const size_t hw = static_cast<size_t>(w) * h;
    img.px.resize(hw * 3);
    for (size_t i = 0; i < hw; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            const float v = chw[c * hw + i];
            const float u = std::isfinite(v) ? std::min(1.0f, std::max(-1.0f, v)) : 0.0f;
            img.px[i * 3 + c] = static_cast<uint8_t>((u + 1.0f) * 127.5f + 0.5f);
        }
    }
    return img;
}

std::vector<uint8_t> EncodeBmp24(const uint8_t* rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> bmp;
    if (!rgb || w == 0 || h == 0 || w > 16384 || h > 16384) return bmp;
    const uint32_t rowBytes = ((w * 3 + 3) / 4) * 4;
    const uint32_t imgSize = rowBytes * h;
    const uint32_t fileSize = 54 + imgSize;
    bmp.assign(fileSize, 0);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) bmp[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i)); };
    auto put16 = [&](size_t at, uint16_t v) { bmp[at] = static_cast<uint8_t>(v); bmp[at + 1] = static_cast<uint8_t>(v >> 8); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h); // positive height: rows are stored bottom-up (the most widely supported form)
    put16(26, 1);
    put16(28, 24);
    put32(34, imgSize);
    put32(38, 2835);
    put32(42, 2835);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = rgb + static_cast<size_t>(h - 1 - y) * w * 3;
        uint8_t* dst = bmp.data() + 54 + static_cast<size_t>(y) * rowBytes;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }
    return bmp;
}

std::vector<uint8_t> EncodePngRgb(const uint8_t* rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out;
    if (!rgb || w == 0 || h == 0 || w > 16384 || h > 16384) return out;
    // Raw scanlines, each prefixed by filter type 0 (None).
    const size_t rowLen = static_cast<size_t>(w) * 3 + 1;
    std::vector<uint8_t> raw(rowLen * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw[static_cast<size_t>(y) * rowLen] = 0;
        std::memcpy(raw.data() + static_cast<size_t>(y) * rowLen + 1, rgb + static_cast<size_t>(y) * w * 3, static_cast<size_t>(w) * 3);
    }
    // zlib stream made of "stored" (uncompressed) deflate blocks: valid PNG, no compressor needed.
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = (pos + n >= raw.size());
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF));
        z.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        z.push_back(static_cast<uint8_t>(~n & 0xFF));
        z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    PutU32(z, (b << 16) | a);

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    out.insert(out.end(), sig, sig + 8);
    std::vector<uint8_t> ihdr;
    PutU32(ihdr, w);
    PutU32(ihdr, h);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(2); // colour type: truecolour RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PutChunk(out, "IHDR", ihdr);
    PutChunk(out, "IDAT", z);
    PutChunk(out, "IEND", std::vector<uint8_t>());
    return out;
}

} // namespace XboxOTFM
