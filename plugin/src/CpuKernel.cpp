// The stitch, seam, blur and tag kernels on the CPU, for Windows. A line-by-line port of the
// Metal shaders of the macOS original (MetalKernel.mm in 0.4.1), in single precision like the
// GPU, spread over all cores. The decoded full resolution lenses are sampled with mip levels
// and anisotropic filtering in software, the way the GPU's sampler did.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "ClipTag.h"
#include "LensDecoder.h"
#include "Parallel.h"
#include "StitchKernel.h"

namespace
{
const int LENS_SLOTS = eLensSlotCount;
static_assert(eLensSlotCount == 24, "lens slots");

// Seam analysis; tools/stitch_poc.py (seam_tables) uses the same constants.
const int SEAM_COLUMNS = 256;        // samples around the seam ...
const int SEAM_ROWS = 32;            // ... and across it, within +-SEAM_BAND_DEG (the lenses overlap about +-6)
const float SEAM_BAND_DEG = 4.0f;
const int SEAM_BINS = kSeamBins;     // table entries around the seam; entry SEAM_BINS holds the global gain
const float SEAM_FADE_DEG = 30.0f;   // the corrections fade out between the band edge and this distance from the seam
const float GAIN_SIGMA_DEG = 15.0f;  // smoothing of the gain along the seam
const float GAIN_PRIOR = 64.0f;      // pulls entries with few usable samples towards the global gain
const int DEPTH_STEPS = 18;          // candidate stitch distances, evenly spaced in 1 / distance ...
const float MAX_INV_DISTANCE = 4.0f; // ... from infinity to 0.25 m (cameras set down in grass, on a stick close to things)
const float DEPTH_SIGMA_DEG = 12.0f; // the match of each candidate is pooled over this much of the seam
const float DEPTH_PRIOR = 0.02f;     // cost per 1/m away from DEFAULT_INV_DISTANCE: keeps flat areas put and frames steady
                                     // (0.05 in 0.4.1 held textured ground near the camera at about 2 m)
const float DEFAULT_INV_DISTANCE = 0.5f;
const float DEPTH_TABLE_SIGMA_DEG = 4.0f;
const int MATCH_GRID = 32;           // points per side and lens where the decoded lenses are compared with the input
const float MATCH_MAX_DIFF = 0.04f;  // mean brightness difference above which the decoded frame is not the input's
const int MAX_ANISOTROPY = 8;
const float PI_F = 3.14159265358979323846f;

// ---- small vector types, as in the shading language -------------------------------------

struct float2
{
    float x, y;
};
struct float3
{
    float x, y, z;
};
struct float4
{
    float x, y, z, w;
};

inline float2 operator+(float2 a, float2 b) { return {a.x + b.x, a.y + b.y}; }
inline float2 operator-(float2 a, float2 b) { return {a.x - b.x, a.y - b.y}; }
inline float2 operator*(float2 a, float2 b) { return {a.x * b.x, a.y * b.y}; }
inline float2 operator/(float2 a, float2 b) { return {a.x / b.x, a.y / b.y}; }
inline float2 operator*(float2 a, float s) { return {a.x * s, a.y * s}; }
inline float2 operator+(float2 a, float s) { return {a.x + s, a.y + s}; }
inline float2 operator-(float2 a, float s) { return {a.x - s, a.y - s}; }
inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float length(float2 a) { return std::sqrt(dot(a, a)); }

inline float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float3 operator/(float3 a, float3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator*(float s, float3 a) { return {a.x * s, a.y * s, a.z * s}; }
inline float3 operator/(float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float3 normalize(float3 a) { return a * (1.0f / std::sqrt(dot(a, a))); }
inline float3 max3(float3 a, float s) { return {std::max(a.x, s), std::max(a.y, s), std::max(a.z, s)}; }
inline float3 clamp3(float3 a, float lo, float hi)
{
    return {std::min(std::max(a.x, lo), hi), std::min(std::max(a.y, lo), hi), std::min(std::max(a.z, lo), hi)};
}
inline float3 sqrt3(float3 a) { return {std::sqrt(a.x), std::sqrt(a.y), std::sqrt(a.z)}; }

inline float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline float4 operator*(float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float4 mix(float4 a, float4 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}; }
inline float3 rgb(float4 a) { return {a.x, a.y, a.z}; }

inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline float smoothstep(float e0, float e1, float x)
{
    const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- where the lens pictures come from -----------------------------------------------------

// Resolve's input buffer, or with fullRes the lens files decoded at full resolution.
struct Source
{
    bool decoded; // fullRes and the decoded frame matches the input; otherwise read the input
    const FrameBuffer* input;
    const LensImages* lenses;
};

inline float2 LensSize(const StitchParams& p)
{
    return p.fullRes != 0 ? float2{float(p.lensWidth), float(p.lensHeight)} : float2{float(p.width / 2), float(p.height)};
}

inline float4 ReadPixel(const FrameBuffer& p_In, int p_Row, int p_X)
{
    const float* px = reinterpret_cast<const float*>(p_In.data + long(p_Row) * p_In.rowBytes) + p_X * 4;
    return {px[0], px[1], px[2], px[3]};
}

// Bilinear sample of one lens tile; x, y are pixel-centre coordinates inside the tile, y from the top.
float4 SampleLens(const FrameBuffer& p_In, const StitchParams& p, int p_Lens, float2 p_Pos)
{
    const int tileW = p.width / 2;
    const float bx = std::floor(p_Pos.x), by = std::floor(p_Pos.y);
    const float fx = p_Pos.x - bx, fy = p_Pos.y - by;
    float4 sum = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j)
    {
        for (int i = 0; i < 2; ++i)
        {
            const int x = int(bx) + i;
            const int y = int(by) + j;
            if (x < 0 || y < 0 || x >= tileW || y >= p.height)
            {
                continue;
            }
            const int row = p.flipY != 0 ? p.height - 1 - y : y;
            const float w = (i == 0 ? 1.0f - fx : fx) * (j == 0 ? 1.0f - fy : fy);
            sum = sum + ReadPixel(p_In, row, p_Lens * tileW + x) * w;
        }
    }
    return sum;
}

// ---- the GPU sampler, in software ----------------------------------------------------------

const float kSampleScale = 1.0f / 65535.0f;

// Bilinear, clamped to the edge, at texel coordinates of one level (texel centres on integers).
inline void Bilinear(const MipLevel& p_Level, int p_Channels, float p_X, float p_Y, float p_Out[2])
{
    const int w = p_Level.width, h = p_Level.height;
    const float x = clampf(p_X, 0.0f, float(w - 1)), y = clampf(p_Y, 0.0f, float(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const uint16_t* r0 = p_Level.data.data() + size_t(y0) * w * p_Channels;
    const uint16_t* r1 = p_Level.data.data() + size_t(y1) * w * p_Channels;
    for (int c = 0; c < p_Channels; ++c)
    {
        const float top = float(r0[x0 * p_Channels + c]) + (float(r0[x1 * p_Channels + c]) - float(r0[x0 * p_Channels + c])) * fx;
        const float bottom = float(r1[x0 * p_Channels + c]) + (float(r1[x1 * p_Channels + c]) - float(r1[x0 * p_Channels + c])) * fx;
        p_Out[c] = (top + (bottom - top) * fy) * kSampleScale;
    }
}

// Linear between mip levels at normalised position p_Uv, mip level p_Lod (>= 0).
inline void Trilinear(const MipChain& p_Chain, float2 p_Uv, float p_Lod, float p_Out[2])
{
    const int last = int(p_Chain.levels.size()) - 1;
    const float lod = clampf(p_Lod, 0.0f, float(last));
    const int l0 = int(lod);
    const int l1 = std::min(l0 + 1, last);
    const float f = lod - float(l0);
    const MipLevel& a = p_Chain.levels[l0];
    Bilinear(a, p_Chain.channels, p_Uv.x * a.width - 0.5f, p_Uv.y * a.height - 0.5f, p_Out);
    if (f > 0.0f && l1 != l0)
    {
        const MipLevel& b = p_Chain.levels[l1];
        float other[2];
        Bilinear(b, p_Chain.channels, p_Uv.x * b.width - 0.5f, p_Uv.y * b.height - 0.5f, other);
        for (int c = 0; c < p_Chain.channels; ++c)
        {
            p_Out[c] += (other[c] - p_Out[c]) * f;
        }
    }
}

// Sample with the footprint of an output pixel, given as two gradients of the normalised
// position, the way a GPU does anisotropic filtering: several trilinear samples along the
// longer axis of the footprint, at the mip level of its shorter axis.
void SampleGrad(const MipChain& p_Chain, float2 p_Uv, float2 p_Dx, float2 p_Dy, float p_Out[2])
{
    const MipLevel& base = p_Chain.levels[0];
    const float2 size = {float(base.width), float(base.height)};
    const float2 dx = p_Dx * size, dy = p_Dy * size; // in texels of level 0
    const float lx = length(dx), ly = length(dy);
    const float major = std::max(lx, ly), minor = std::min(lx, ly);
    if (!(major > 1.0f))
    {
        Trilinear(p_Chain, p_Uv, 0.0f, p_Out); // magnified: plain bilinear
        return;
    }
    const int n = std::min(MAX_ANISOTROPY, std::max(1, int(std::ceil(major / std::max(minor, 1e-6f)))));
    const float lod = std::max(0.0f, std::log2(major / float(n)));
    if (n == 1)
    {
        Trilinear(p_Chain, p_Uv, lod, p_Out);
        return;
    }
    const float2 axis = (lx >= ly ? p_Dx : p_Dy);
    float sum[2] = {0.0f, 0.0f};
    for (int i = 0; i < n; ++i)
    {
        const float t = (float(i) + 0.5f) / float(n) - 0.5f;
        float s[2];
        Trilinear(p_Chain, p_Uv + axis * t, lod, s);
        for (int c = 0; c < p_Chain.channels; ++c)
        {
            sum[c] += s[c];
        }
    }
    for (int c = 0; c < p_Chain.channels; ++c)
    {
        p_Out[c] = sum[c] / float(n);
    }
}

// Sample of a decoded lens at pixel-centre position p_Pos (y from the top), converted to RGB.
// With p_Lod < 0 the mip level comes from the footprint p_Dx, p_Dy of an output pixel in lens
// pixels, with anisotropic filtering; otherwise it is mip level p_Lod.
float3 SampleDecoded(const MipChain& p_Luma, const MipChain& p_Chroma, const StitchParams& p, float2 p_Pos, float2 p_Dx, float2 p_Dy,
                     float p_Lod)
{
    const float2 size = LensSize(p);
    const float2 uv = (p_Pos + 0.5f) / size;
    // HEVC 4:2:0 chroma sits on even columns, between rows.
    const float2 uvChroma = (float2{p_Pos.x * 0.5f, (p_Pos.y - 0.5f) * 0.5f} + 0.5f) / (size * 0.5f);
    float y[2], c[2];
    if (p_Lod < 0.0f)
    {
        SampleGrad(p_Luma, uv, p_Dx / size, p_Dy / size, y);
        SampleGrad(p_Chroma, uvChroma, p_Dx / size, p_Dy / size, c);
    }
    else
    {
        Trilinear(p_Luma, uv, p_Lod, y);
        Trilinear(p_Chroma, uvChroma, p_Lod, c);
    }
    // Full range BT.709, as the X5 records.
    const float luma = y[0] * p.yuvScale;
    const float cb = c[0] * p.yuvScale - p.chromaMid, cr = c[1] * p.yuvScale - p.chromaMid;
    return {luma + 1.5748f * cr, luma - 0.187324f * cb - 0.468124f * cr, luma + 1.8556f * cb};
}

// Position in a lens tile of the input buffer for a position in a decoded lens.
inline float2 InputPos(const StitchParams& p, float2 p_Pos)
{
    return (p_Pos + 0.5f) * float2{float(p.width / 2), float(p.height)} / LensSize(p) - 0.5f;
}

float3 SampleSource(const Source& p_Src, const StitchParams& p, int p_Lens, float2 p_Pos, float2 p_Dx, float2 p_Dy, float p_Lod)
{
    if (p.fullRes == 0)
    {
        return rgb(SampleLens(*p_Src.input, p, p_Lens, p_Pos));
    }
    if (!p_Src.decoded)
    {
        return rgb(SampleLens(*p_Src.input, p, p_Lens, InputPos(p, p_Pos)));
    }
    const MipChain& luma = *p_Src.lenses->planes[p_Lens * 2];
    const MipChain& chroma = *p_Src.lenses->planes[p_Lens * 2 + 1];
    return SampleDecoded(luma, chroma, p, p_Pos, p_Dx, p_Dy, p_Lod);
}

// For the seam analysis, which is tuned to pictures about 1000 pixels across.
inline float3 SeamSample(const Source& p_Src, const StitchParams& p, int p_Lens, float2 p_Pos)
{
    return SampleSource(p_Src, p, p_Lens, p_Pos, float2{0, 0}, float2{0, 0}, p.seamLod);
}

// ---- geometry -------------------------------------------------------------------------------

// Projects a rig-space ray into a lens tile (MEI fisheye model). Returns false outside the image circle.
bool ProjectToLens(const StitchParams& p, int p_Lens, float3 p_Ray, float2& p_Pos)
{
    const int o = p_Lens * LENS_SLOTS;
    const float* l = p.lens;
    const float3 local = {dot(float3{l[o + 15], l[o + 16], l[o + 17]}, p_Ray), dot(float3{l[o + 18], l[o + 19], l[o + 20]}, p_Ray),
                          dot(float3{l[o + 21], l[o + 22], l[o + 23]}, p_Ray)};
    const float den = local.z + l[o + 4];
    if (den <= 1e-6f)
    {
        return false;
    }

    const float x = local.x / den;
    const float y = local.y / den;
    const float r2 = x * x + y * y;
    const float r4 = r2 * r2;
    const float k1 = l[o + 5], k2 = l[o + 6], k3 = l[o + 7], k4 = l[o + 8];
    const float p1 = l[o + 9], p2 = l[o + 10];
    const float s1 = l[o + 11], s2 = l[o + 12], s3 = l[o + 13], s4 = l[o + 14];
    const float radial = 1.0f + k1 * r2 + k2 * r4 + k3 * r2 * r4 + k4 * r4 * r4;
    const float xd = x * radial + 2.0f * p1 * x * y + p2 * (r2 + 2.0f * x * x) + s1 * r2 + s2 * r4;
    const float yd = y * radial + p1 * (r2 + 2.0f * y * y) + 2.0f * p2 * x * y + s3 * r2 + s4 * r4;

    p_Pos = {l[o + 0] * xd + l[o + 2], l[o + 1] * yd + l[o + 3]};

    // Image circle test, per axis so that squeezed frames work too.
    const float2 radius = LensSize(p) * 0.5f;
    return length((p_Pos - radius) / radius) < 0.975f;
}

// Direction from lens A (p_Sign 1) or lens B (-1) to the point at 1 / p_InvDistance metres along
// p_Ray. The rig centre sits halfway between the lenses, so each lens takes half the parallax.
inline float3 LensRay(const StitchParams& p, float3 p_Ray, float p_InvDistance, float p_Sign)
{
    const float3 halfOffset = float3{p.lensOffset[0], p.lensOffset[1], p.lensOffset[2]} * 0.5f;
    return normalize(p_Ray + halfOffset * (p_Sign * p_InvDistance));
}

inline float3 SeamRay(int p_Column, int p_Row)
{
    const float phi = (float(p_Column) + 0.5f) / float(SEAM_COLUMNS) * 2.0f * PI_F;
    const float z = std::sin(SEAM_BAND_DEG * PI_F / 180.0f) * ((float(p_Row) + 0.5f) / float(SEAM_ROWS) * 2.0f - 1.0f);
    const float s = std::sqrt(1.0f - z * z);
    return {s * std::cos(phi), s * std::sin(phi), z};
}

// Angle between two seam columns, wrapped to -pi..pi.
inline float ColumnAngle(int p_A, int p_B)
{
    const float d = float(p_A - p_B) / float(SEAM_COLUMNS) * 2.0f * PI_F;
    return d - 2.0f * PI_F * std::nearbyint(d / (2.0f * PI_F));
}

// Measures the seam once per frame and fills a table around it: per-channel gain lens A / lens B
// in rgb and 1 / stitch distance in w, plus the global gain in entry SEAM_BINS and the result of
// the full resolution frame check in entry SEAM_BINS + 1.
// 1. stitch distance per column: the candidate where the lenses agree best across the seam,
//    pooled along the seam, with a small pull towards the default distance;
// 2. colour of both lenses at that distance, leaving out near black, clipped and mismatched samples.
void SeamPass(const StitchParams& p, const FrameBuffer& p_Input, const LensImages* p_Lenses, float4* p_Table)
{
    // With fullRes, first check that the decoded frame is the one in the input: the host's time
    // is not always the frame in the file (in Fusion it counts from the start of the comp).
    // Both are compared at the input's resolution, inside the image circles.
    bool decoded = false;
    if (p.fullRes != 0 && p.trustDecoded != 0)
    {
        decoded = true;
        p_Table[SEAM_BINS + 1] = {1.0f, 0.0f, 0.0f, 0.0f};
    }
    else if (p.fullRes != 0)
    {
        const Source probe = {true, &p_Input, p_Lenses};
        const float2 size = LensSize(p);
        const float2 footprint = size / float2{float(p.width / 2), float(p.height)};
        const int points = 2 * MATCH_GRID * MATCH_GRID;
        std::vector<float> diffs(points, -1.0f);
        ParallelFor(points, 64, [&](int i) {
            const int lens = i / (MATCH_GRID * MATCH_GRID);
            const int cell = i % (MATCH_GRID * MATCH_GRID);
            const float2 uv = (float2{float(cell % MATCH_GRID), float(cell / MATCH_GRID)} + 0.5f) * (1.0f / MATCH_GRID);
            if (length(uv * 2.0f - 1.0f) > 0.85f)
            {
                return;
            }
            const float2 pos = uv * size - 0.5f;
            const float3 weights = {0.2126f, 0.7152f, 0.0722f};
            const float a = dot(SampleSource(probe, p, lens, pos, float2{footprint.x, 0.0f}, float2{0.0f, footprint.y}, -1.0f), weights);
            const float b = dot(rgb(SampleLens(p_Input, p, lens, InputPos(p, pos))), weights);
            diffs[i] = std::fabs(a - b);
        });
        double sum = 0.0, count = 0.0;
        for (float d : diffs)
        {
            if (d >= 0.0f)
            {
                sum += d;
                count += 1.0;
            }
        }
        const float diff = float(sum / std::max(count, 1.0));
        decoded = diff < MATCH_MAX_DIFF;
        p_Table[SEAM_BINS + 1] = {decoded ? 1.0f : 0.0f, diff, 0.0f, 0.0f};
    }
    else
    {
        p_Table[SEAM_BINS + 1] = {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const Source src = {decoded, &p_Input, p_Lenses};

    std::vector<float> colQ(SEAM_COLUMNS);
    const float stepQ = MAX_INV_DISTANCE / float(DEPTH_STEPS - 1);
    if (p.autoDistance != 0)
    {
        std::vector<float> costSum(SEAM_COLUMNS * DEPTH_STEPS);
        std::vector<int> costCount(SEAM_COLUMNS * DEPTH_STEPS);
        ParallelFor(SEAM_COLUMNS, 4, [&](int c) {
            for (int k = 0; k < DEPTH_STEPS; ++k)
            {
                float sum = 0.0f;
                int n = 0;
                for (int r = 0; r < SEAM_ROWS; ++r)
                {
                    const float3 ray = SeamRay(c, r);
                    float2 posA, posB;
                    if (ProjectToLens(p, 0, LensRay(p, ray, float(k) * stepQ, 1.0f), posA) &&
                        ProjectToLens(p, 1, LensRay(p, ray, float(k) * stepQ, -1.0f), posB))
                    {
                        const float3 a = SeamSample(src, p, 0, posA);
                        const float3 b = SeamSample(src, p, 1, posB);
                        sum += std::fabs((a.x + a.y + a.z) - (b.x + b.y + b.z)) / 3.0f;
                        ++n;
                    }
                }
                costSum[c * DEPTH_STEPS + k] = sum;
                costCount[c * DEPTH_STEPS + k] = n;
            }
        });

        const float sigma = DEPTH_SIGMA_DEG * PI_F / 180.0f;
        ParallelFor(SEAM_COLUMNS, 16, [&](int c) {
            float sum[DEPTH_STEPS], n[DEPTH_STEPS];
            for (int k = 0; k < DEPTH_STEPS; ++k)
            {
                sum[k] = 0.0f;
                n[k] = 0.0f;
            }
            for (int o = 0; o < SEAM_COLUMNS; ++o)
            {
                const float d = ColumnAngle(o, c) / sigma;
                const float w = std::exp(-0.5f * d * d);
                for (int k = 0; k < DEPTH_STEPS; ++k)
                {
                    sum[k] += w * costSum[o * DEPTH_STEPS + k];
                    n[k] += w * float(costCount[o * DEPTH_STEPS + k]);
                }
            }
            float cost[DEPTH_STEPS];
            int best = 0;
            for (int k = 0; k < DEPTH_STEPS; ++k)
            {
                cost[k] = sum[k] / std::max(n[k], 1e-6f) + DEPTH_PRIOR * std::fabs(float(k) * stepQ - DEFAULT_INV_DISTANCE);
                best = cost[k] < cost[best] ? k : best;
            }
            // Refine between candidates with a parabola through the best one and its neighbours.
            float shift = 0.0f;
            if (best > 0 && best < DEPTH_STEPS - 1)
            {
                const float curve = cost[best - 1] - 2.0f * cost[best] + cost[best + 1];
                shift = curve > 1e-9f ? clampf(0.5f * (cost[best - 1] - cost[best + 1]) / curve, -0.5f, 0.5f) : 0.0f;
            }
            colQ[c] = (float(best) + shift) * stepQ;
        });
    }
    else
    {
        std::fill(colQ.begin(), colQ.end(), p.invDistance);
    }

    std::vector<float4> colA(SEAM_COLUMNS), colB(SEAM_COLUMNS);
    ParallelFor(SEAM_COLUMNS, 8, [&](int c) {
        float4 sumA = {0, 0, 0, 0}, sumB = {0, 0, 0, 0};
        for (int r = 0; r < SEAM_ROWS && p.gainMatch != 0; ++r)
        {
            const float3 ray = SeamRay(c, r);
            float2 posA, posB;
            if (!ProjectToLens(p, 0, LensRay(p, ray, colQ[c], 1.0f), posA) || !ProjectToLens(p, 1, LensRay(p, ray, colQ[c], -1.0f), posB))
            {
                continue;
            }
            const float3 a = SeamSample(src, p, 0, posA);
            const float3 b = SeamSample(src, p, 1, posB);
            const float la = (a.x + a.y + a.z) / 3.0f, lb = (b.x + b.y + b.z) / 3.0f;
            if (la > 0.02f && lb > 0.02f && la < 0.95f && lb < 0.95f && la < 2.0f * lb && lb < 2.0f * la)
            {
                sumA = sumA + float4{a.x, a.y, a.z, 1.0f};
                sumB = sumB + float4{b.x, b.y, b.z, 1.0f};
            }
        }
        colA[c] = sumA;
        colB[c] = sumB;
    });

    float4 totalA = {0, 0, 0, 0}, totalB = {0, 0, 0, 0};
    for (int c = 0; c < SEAM_COLUMNS; ++c)
    {
        totalA = totalA + colA[c];
        totalB = totalB + colB[c];
    }
    const bool measured = totalA.w > 0.0f;
    const float3 meanA = rgb(totalA) / std::max(totalA.w, 1.0f), meanB = rgb(totalB) / std::max(totalB.w, 1.0f);
    const float3 overall = measured ? clamp3(meanA / max3(meanB, 1e-6f), 0.5f, 2.0f) : float3{1, 1, 1};
    const float gainSigma = GAIN_SIGMA_DEG * PI_F / 180.0f, depthSigma = DEPTH_TABLE_SIGMA_DEG * PI_F / 180.0f;
    for (int k = 0; k < SEAM_BINS; ++k)
    {
        const float centre = (float(k) + 0.5f) / float(SEAM_BINS) * 2.0f * PI_F;
        float3 sumA = meanA * GAIN_PRIOR, sumB = meanB * GAIN_PRIOR;
        float q = 0.0f, qWeight = 0.0f;
        for (int c = 0; c < SEAM_COLUMNS; ++c)
        {
            float d = (float(c) + 0.5f) / float(SEAM_COLUMNS) * 2.0f * PI_F - centre;
            d -= 2.0f * PI_F * std::nearbyint(d / (2.0f * PI_F));
            const float w = std::exp(-0.5f * (d / gainSigma) * (d / gainSigma));
            sumA = sumA + rgb(colA[c]) * w;
            sumB = sumB + rgb(colB[c]) * w;
            const float wq = std::exp(-0.5f * (d / depthSigma) * (d / depthSigma));
            q += wq * colQ[c];
            qWeight += wq;
        }
        const float3 gain = measured ? clamp3(sumA / max3(sumB, 1e-6f), 0.5f, 2.0f) : float3{1, 1, 1};
        p_Table[k] = {gain.x, gain.y, gain.z, q / qWeight};
    }
    p_Table[SEAM_BINS] = {overall.x, overall.y, overall.z, 0.0f};
}

// Table value around the seam for a ray.
inline float4 TableAt(const float4* p_Table, float3 p_Ray)
{
    float f = std::atan2(p_Ray.y, p_Ray.x) / (2.0f * PI_F) * float(SEAM_BINS) - 0.5f;
    f -= float(SEAM_BINS) * std::floor(f / float(SEAM_BINS));
    const float k = std::floor(f);
    const int k0 = int(k) % SEAM_BINS;
    return mix(p_Table[k0], p_Table[(k0 + 1) % SEAM_BINS], f - k);
}

// 1 within the measured band around the seam, fading to 0 at SEAM_FADE_DEG from it.
inline float NearSeam(float3 p_Ray)
{
    const float lo = std::sin(SEAM_BAND_DEG * PI_F / 180.0f), hi = std::sin(SEAM_FADE_DEG * PI_F / 180.0f);
    return clampf((hi - std::fabs(p_Ray.z)) / (hi - lo), 0.0f, 1.0f);
}

// Direction in the rig frame (x right, y down, z forward) seen at an output pixel, x and y from the
// top left, stabilisation applied.
float3 OutputRay(const StitchParams& p, float2 p_Pixel)
{
    const float2 uv = (p_Pixel + 0.5f) / float2{float(p.width), float(p.height)};
    float3 ray;
    if (p.mode == eModeStitch)
    {
        // Equirectangular: the whole sphere.
        const float lon = uv.x * 2.0f * PI_F - PI_F;
        const float lat = PI_F / 2.0f - uv.y * PI_F;
        ray = {std::cos(lat) * std::sin(lon), -std::sin(lat), std::cos(lat) * std::cos(lon)};
    }
    else
    {
        // Virtual camera "distance" behind the sphere centre, looking along +z: follow its ray
        // through this point of the image plane to the sphere, then rotate into the rig frame.
        const float2 plane = (uv * 2.0f - 1.0f) * float2{p.planeX, p.planeY};
        const float d = p.distance;
        const float r2 = dot(plane, plane);
        const float t = (d + std::sqrt(std::max(1.0f + r2 * (1.0f - d * d), 0.0f))) / (r2 + 1.0f);
        const float3 cam = {plane.x * t, plane.y * t, t - d}; // unit length
        ray = {dot(float3{p.camRot[0], p.camRot[1], p.camRot[2]}, cam), dot(float3{p.camRot[3], p.camRot[4], p.camRot[5]}, cam),
               dot(float3{p.camRot[6], p.camRot[7], p.camRot[8]}, cam)};
    }

    if (p.stabilize != 0)
    {
        // Stabilisation: the view stays put while the camera moves, so turn the ray into the camera.
        ray = {dot(float3{p.stab[0], p.stab[1], p.stab[2]}, ray), dot(float3{p.stab[3], p.stab[4], p.stab[5]}, ray),
               dot(float3{p.stab[6], p.stab[7], p.stab[8]}, ray)};
    }
    return ray;
}

// One output pixel of the stitched or reframed picture.
float4 StitchPixel(const StitchParams& p, const Source& src, const float4* p_Table, int p_X, int p_Row)
{
    // Output pixel -> direction in the rig frame (x right, y down, z forward).
    const int yTop = p.flipY != 0 ? p.height - 1 - p_Row : p_Row;
    const float3 ray = OutputRay(p, float2{float(p_X), float(yTop)});

    // Each lens looks at the point at the stitch distance, so that things at that distance line up.
    const float4 seam = TableAt(p_Table, ray);
    const float near = NearSeam(ray);
    float2 posA, posB;
    const bool okA = ProjectToLens(p, 0, LensRay(p, ray, seam.w * near, 1.0f), posA);
    const bool okB = ProjectToLens(p, 1, LensRay(p, ray, seam.w * near, -1.0f), posB);

    // Lens A looks along +z; cross-fade in a narrow band around the z = 0 seam. Smoothstep,
    // so that the fade has no kink at the band edges.
    float wA = smoothstep(-p.blendBand, p.blendBand, ray.z);
    wA = okA ? (okB ? wA : 1.0f) : 0.0f;
    const float wB = okB ? (okA ? 1.0f - wA : 1.0f) : 0.0f;

    // Meet in the middle: lens A down by the square root of the gain, lens B up by it.
    const float3 overall = rgb(p_Table[SEAM_BINS]);
    const float3 root = p.gainMatch != 0 ? sqrt3(overall + (rgb(seam) - overall) * near) : float3{1, 1, 1};
    // Footprint of this output pixel in each lens, for filtering the full resolution pictures:
    // where the neighbouring pixels land.
    float2 dxA = {0, 0}, dyA = {0, 0}, dxB = {0, 0}, dyB = {0, 0};
    if (src.decoded)
    {
        const float3 rayX = OutputRay(p, float2{float(p_X) + 1.0f, float(yTop)});
        const float3 rayY = OutputRay(p, float2{float(p_X), float(yTop) + 1.0f});
        float2 q;
        dxA = okA && ProjectToLens(p, 0, LensRay(p, rayX, seam.w * near, 1.0f), q) ? q - posA : float2{0, 0};
        dyA = okA && ProjectToLens(p, 0, LensRay(p, rayY, seam.w * near, 1.0f), q) ? q - posA : float2{0, 0};
        dxB = okB && ProjectToLens(p, 1, LensRay(p, rayX, seam.w * near, -1.0f), q) ? q - posB : float2{0, 0};
        dyB = okB && ProjectToLens(p, 1, LensRay(p, rayY, seam.w * near, -1.0f), q) ? q - posB : float2{0, 0};
    }
    float3 sum = {0, 0, 0};
    if (wA > 0.0f)
    {
        sum = sum + SampleSource(src, p, 0, posA, dxA, dyA, -1.0f) / root * wA;
    }
    if (wB > 0.0f)
    {
        sum = sum + SampleSource(src, p, 1, posB, dxB, dyB, -1.0f) * root * wB;
    }
    const float3 out = sum / std::max(wA + wB, 1e-6f);
    return {out.x, out.y, out.z, 1.0f};
}

// One row of the stitched or reframed picture from the decoded lenses. Same result as
// StitchPixel, but each pixel's footprint (for filtering) comes from its neighbours in the row,
// which are computed anyway, and from the row below at every kFootprintStep-th pixel, instead
// of from two extra rays per pixel: the geometry is most of the work.
const int kFootprintStep = 4;

struct RowPoint
{
    float3 ray;
    float4 seam;
    float near;
    float2 pos[2];
    bool ok[2];
    bool have[2]; // pos and ok of that lens are worked out
};

// The ray of an output point; its lens positions follow on demand (Have).
void StartPoint(const StitchParams& p, const float4* p_Table, float p_X, float p_Y, RowPoint& p_Out)
{
    p_Out.ray = OutputRay(p, float2{p_X, p_Y});
    p_Out.near = NearSeam(p_Out.ray);
    // Beyond SEAM_FADE_DEG from the seam the table has no effect (near is 0): skip the lookup.
    p_Out.seam = p_Out.near > 0.0f ? TableAt(p_Table, p_Out.ray) : float4{0, 0, 0, 0};
    p_Out.have[0] = p_Out.have[1] = false;
}

inline void Have(const StitchParams& p, RowPoint& p_Point, int p_Lens)
{
    if (!p_Point.have[p_Lens])
    {
        p_Point.ok[p_Lens] = ProjectToLens(p, p_Lens, LensRay(p, p_Point.ray, p_Point.seam.w * p_Point.near, p_Lens == 0 ? 1.0f : -1.0f),
                                           p_Point.pos[p_Lens]);
        p_Point.have[p_Lens] = true;
    }
}

void StitchRowDecoded(const StitchParams& p, const Source& src, const float4* p_Table, int p_Row, int p_X1, int p_X2,
                      std::vector<RowPoint>& p_Points, const FrameOutput& p_Out)
{
    const int yTop = p.flipY != 0 ? p.height - 1 - p_Row : p_Row;
    const int n = p_X2 - p_X1;
    p_Points.resize(size_t(n) + 1);
    for (int i = 0; i <= n; ++i)
    {
        StartPoint(p, p_Table, float(p_X1 + i), float(yTop), p_Points[i]);
    }
    const float3 overall = rgb(p_Table[SEAM_BINS]);
    // The step to the row below, measured at every kFootprintStep-th pixel and used for the
    // pixels up to the next one (it changes slowly along a row).
    RowPoint below;
    int belowAt = -1;
    float2 dyAt[2] = {{0, 0}, {0, 0}};
    bool dyKnown[2] = {false, false};
    for (int i = 0; i < n; ++i)
    {
        RowPoint& pt = p_Points[i];
        RowPoint& right = p_Points[i + 1];
        // Away from the seam one lens decides the pixel; the other is only needed where that one
        // does not see it, or within the blend band (same weights as StitchPixel).
        const int main = pt.ray.z >= 0.0f ? 0 : 1, other = 1 - main;
        Have(p, pt, main);
        if (!pt.ok[main] || std::fabs(pt.ray.z) < p.blendBand)
        {
            Have(p, pt, other);
        }
        else
        {
            pt.ok[other] = false; // its weight would be 0
        }
        const bool okA = pt.ok[0], okB = pt.ok[1];
        float wA = smoothstep(-p.blendBand, p.blendBand, pt.ray.z);
        wA = okA ? (okB ? wA : 1.0f) : 0.0f;
        const float wB = okB ? (okA ? 1.0f - wA : 1.0f) : 0.0f;
        if (i - belowAt >= kFootprintStep || belowAt < 0)
        {
            StartPoint(p, p_Table, float(p_X1 + i), float(yTop) + 1.0f, below);
            belowAt = i;
            dyKnown[0] = dyKnown[1] = false;
        }
        const float3 root = p.gainMatch != 0 ? sqrt3(overall + (rgb(pt.seam) - overall) * pt.near) : float3{1, 1, 1};
        float3 sum = {0, 0, 0};
        const float w[2] = {wA, wB};
        for (int l = 0; l < 2; ++l)
        {
            if (w[l] <= 0.0f)
            {
                continue;
            }
            Have(p, right, l);
            if (!dyKnown[l])
            {
                RowPoint& above = p_Points[belowAt];
                Have(p, above, l);
                Have(p, below, l);
                dyAt[l] = above.ok[l] && below.ok[l] ? below.pos[l] - above.pos[l] : float2{0, 0};
                dyKnown[l] = true;
            }
            const float2 dx = right.ok[l] ? right.pos[l] - pt.pos[l] : float2{0, 0};
            const float2 dy = dyAt[l];
            const float3 c = SampleSource(src, p, l, pt.pos[l], dx, dy, -1.0f);
            sum = sum + (l == 0 ? c / root : c * root) * w[l];
        }
        const float3 out = sum / std::max(wA + wB, 1e-6f);
        float* px = reinterpret_cast<float*>(p_Out.data + long(p_Row) * p_Out.rowBytes) + (p_X1 + i) * 4;
        px[0] = out.x;
        px[1] = out.y;
        px[2] = out.z;
        px[3] = 1.0f;
    }
}

inline void WritePixel(const FrameOutput& p_Out, int p_Row, int p_X, float4 p_Px)
{
    float* px = reinterpret_cast<float*>(p_Out.data + long(p_Row) * p_Out.rowBytes) + p_X * 4;
    px[0] = p_Px.x;
    px[1] = p_Px.y;
    px[2] = p_Px.z;
    px[3] = p_Px.w;
}
}

int ReadClipTag(const StitchParams& p_Params, const FrameBuffer& p_Input)
{
    if (!p_Input.data || p_Params.width <= 0 || p_Params.height <= 0)
    {
        return -1;
    }
    float brightness[ClipTag::kBits];
    for (int bit = 0; bit < ClipTag::kBits; ++bit)
    {
        int x, y;
        ClipTag::CellCentre(bit, p_Params.width, p_Params.height, x, y);
        x = std::min(std::max(x, 0), p_Params.width - 1);
        y = std::min(std::max(y, 0), p_Params.height - 1);
        const int row = p_Params.flipY != 0 ? p_Params.height - 1 - y : y;
        const float4 px = ReadPixel(p_Input, row, x);
        brightness[bit] = px.x * 0.299f + px.y * 0.587f + px.z * 0.114f;
    }
    return ClipTag::Decode(brightness);
}

bool RunStitchKernel(const StitchParams& p_Params, const FrameBuffer& p_Input, const FrameOutput& p_Output, const LensImages* p_Lenses,
                     std::shared_ptr<FullResCheck> p_Check, KernelWindow p_Window)
{
    const StitchParams& p = p_Params;
    if (!p_Input.data || !p_Output.data || p.width <= 0 || p.height <= 0)
    {
        return false;
    }
    StitchParams params = p_Params;
    const bool decoded = params.fullRes != 0 && p_Lenses && p_Lenses->planes[0] && p_Lenses->planes[1] && p_Lenses->planes[2] &&
                         p_Lenses->planes[3];
    params.fullRes = decoded ? 1 : 0;

    KernelWindow win = p_Window;
    if (win.x2 <= win.x1 || win.y2 <= win.y1)
    {
        win = {0, 0, params.width, params.height};
    }
    win.x1 = std::max(win.x1, 0);
    win.y1 = std::max(win.y1, 0);
    win.x2 = std::min(win.x2, params.width);
    win.y2 = std::min(win.y2, params.height);

    if (params.mode == eModeStitch || params.mode == eModeReframe)
    {
        float4 table[kSeamBins + 2] = {};
        SeamPass(params, p_Input, decoded ? p_Lenses : nullptr, table);
        if (decoded && p_Check)
        {
            (table[kSeamBins + 1].x > 0.5f ? p_Check->matched : p_Check->mismatched)++;
            p_Check->lastDiff = table[kSeamBins + 1].y;
        }
        // The seam pass found whether the decoded frame is the input's.
        const Source src = {decoded && table[kSeamBins + 1].x > 0.5f, &p_Input, decoded ? p_Lenses : nullptr};
        if (src.decoded)
        {
            ParallelFor(win.y2 - win.y1, 2, [&](int i) {
                thread_local std::vector<RowPoint> points;
                StitchRowDecoded(params, src, table, win.y1 + i, win.x1, win.x2, points, p_Output);
            });
            return true;
        }
        ParallelFor(win.y2 - win.y1, 2, [&](int i) {
            const int row = win.y1 + i;
            for (int x = win.x1; x < win.x2; ++x)
            {
                WritePixel(p_Output, row, x, StitchPixel(params, src, table, x, row));
            }
        });
        return true;
    }

    ParallelFor(win.y2 - win.y1, 16, [&](int i) {
        const int row = win.y1 + i;
        for (int x = win.x1; x < win.x2; ++x)
        {
            float4 px = ReadPixel(p_Input, row, x);
            if (params.mode == eModeSourceTint)
            {
                const bool left = x < params.width / 2;
                const float3 tint = left ? float3{1.0f, 0.5f, 0.5f} : float3{0.5f, 0.5f, 1.0f};
                px = {px.x * tint.x, px.y * tint.y, px.z * tint.z, px.w};
                if (row < params.height / 8)
                {
                    px = {px.x * 0.25f, px.y * 0.25f, px.z * 0.25f, px.w}; // marks buffer row 0, to show which way up the host stores frames
                }
            }
            WritePixel(p_Output, row, x, px);
        }
    });
    return true;
}

namespace
{
struct BlurParams
{
    int width;
    int height;
    int wrap;
    float amount;
};

inline float4 ReadBlurPixel(const std::vector<float>& p_Input, const BlurParams& b, int p_X, int p_Y)
{
    const int x = b.wrap != 0 ? ((p_X % b.width) + b.width) % b.width : std::min(std::max(p_X, 0), b.width - 1);
    const int y = std::min(std::max(p_Y, 0), b.height - 1);
    const size_t i = (size_t(y) * b.width + x) * 4;
    return {p_Input[i], p_Input[i + 1], p_Input[i + 2], p_Input[i + 3]};
}

inline float4 ReadBlurBilinear(const std::vector<float>& p_Input, const BlurParams& b, float2 p_Pos)
{
    const float bx = std::floor(p_Pos.x), by = std::floor(p_Pos.y);
    const float fx = p_Pos.x - bx, fy = p_Pos.y - by;
    const int x = int(bx), y = int(by);
    return mix(mix(ReadBlurPixel(p_Input, b, x, y), ReadBlurPixel(p_Input, b, x + 1, y), fx),
               mix(ReadBlurPixel(p_Input, b, x, y + 1), ReadBlurPixel(p_Input, b, x + 1, y + 1), fx), fy);
}

// Linear sample of a flow field, clamped to the edge, at normalised position p_Uv.
inline float2 SampleFlow(const FlowField& p_Flow, float2 p_Uv)
{
    const int w = p_Flow.width, h = p_Flow.height;
    if (p_Flow.xy.size() < size_t(w) * h * 2 || w <= 0 || h <= 0)
    {
        return {0, 0};
    }
    const float x = clampf(p_Uv.x * w - 0.5f, 0.0f, float(w - 1)), y = clampf(p_Uv.y * h - 0.5f, 0.0f, float(h - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    auto at = [&](int px, int py) { return float2{p_Flow.xy[(size_t(py) * w + px) * 2], p_Flow.xy[(size_t(py) * w + px) * 2 + 1]}; };
    const float2 top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float2 bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return top + (bottom - top) * fy;
}
}

bool RunBlurKernel(int p_Width, int p_Height, bool p_Wrap, float p_Amount, const FlowField& p_Previous, const FlowField& p_Next,
                   const FrameOutput& p_Output)
{
    if (!p_Output.data || p_Width <= 0 || p_Height <= 0)
    {
        return false;
    }
    const BlurParams b = {p_Width, p_Height, p_Wrap ? 1 : 0, p_Amount};
    // The blur reads the finished frame, so it works on a copy.
    std::vector<float> copy(size_t(p_Width) * p_Height * 4);
    for (int y = 0; y < p_Height; ++y)
    {
        memcpy(&copy[size_t(y) * p_Width * 4], p_Output.data + long(y) * p_Output.rowBytes, size_t(p_Width) * 16);
    }
    const float2 size = {float(p_Width), float(p_Height)};
    // Averages the picture along the path each pixel takes, half towards the previous frame and
    // half towards the next.
    ParallelFor(p_Height, 4, [&](int y) {
        for (int x = 0; x < p_Width; ++x)
        {
            const float2 uv = (float2{float(x), float(y)} + 0.5f) / size;
            const float2 back = SampleFlow(p_Previous, uv) * size * (0.5f * b.amount);
            const float2 ahead = SampleFlow(p_Next, uv) * size * (0.5f * b.amount);
            const int samples = std::min(std::max(int(length(back) + length(ahead)), 1), 48);
            float4 sum = {0, 0, 0, 0};
            for (int i = 0; i < samples; ++i)
            {
                const float t = (float(i) + 0.5f) / float(samples) * 2.0f - 1.0f;
                sum = sum + ReadBlurBilinear(copy, b, float2{float(x), float(y)} + (t < 0.0f ? back * -t : ahead * t));
            }
            WritePixel(p_Output, y, x, sum * (1.0f / float(samples)));
        }
    });
    return true;
}
