#include "MotionBlur.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "Parallel.h"

namespace
{
const int kFlowSize = 640;     // longest side the movement is measured at
const int kMinLevelSize = 12;  // coarsest pyramid level, shorter side
const int kWindowRadius = 4;   // Lucas-Kanade window: 9 x 9 pixels
const int kIterations = 4;     // warps per pyramid level
const int kSmoothRadius = 4;   // smoothing of the movement per level, two box passes of 9 x 9 pixels
// Smaller eigenvalue of the structure tensor (summed over the window, brightness 0..1) below
// which a window does not measure movement, and at which it counts half when smoothing. A
// corner of 10% contrast gives about 0.02; 8-bit noise and smooth shading stay well below 0.005,
// and there a measurement would be noise that the blur turns into long smears.
const float kMinConfidence = 5e-3f;
const float kConfidenceHalf = 2e-2f;
const float kMaxMotion = 0.15f; // longest believable movement between frames, as a share of the picture's diagonal

struct Plane
{
    int width = 0;
    int height = 0;
    std::vector<float> data;

    float at(int x, int y) const
    {
        x = std::min(std::max(x, 0), width - 1);
        y = std::min(std::max(y, 0), height - 1);
        return data[size_t(y) * width + x];
    }
    float bilinear(float x, float y) const
    {
        x = std::min(std::max(x, 0.0f), float(width - 1));
        y = std::min(std::max(y, 0.0f), float(height - 1));
        const int x0 = int(x), y0 = int(y);
        const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const float fx = x - float(x0), fy = y - float(y0);
        const float* r0 = &data[size_t(y0) * width];
        const float* r1 = &data[size_t(y1) * width];
        const float top = r0[x0] + (r0[x1] - r0[x0]) * fx;
        const float bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
        return top + (bottom - top) * fy;
    }
};

Plane Gray(const unsigned char* p_Bgra, int p_Width, int p_Height)
{
    Plane out;
    out.width = p_Width;
    out.height = p_Height;
    out.data.resize(size_t(p_Width) * p_Height);
    for (size_t i = 0; i < out.data.size(); ++i)
    {
        const unsigned char* px = p_Bgra + i * 4;
        out.data[i] = (0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2]) / 255.0f;
    }
    return out;
}

// Half size, after a [1 2 1] blur in both directions.
Plane Down(const Plane& p_In)
{
    Plane out;
    out.width = std::max(1, p_In.width / 2);
    out.height = std::max(1, p_In.height / 2);
    out.data.resize(size_t(out.width) * out.height);
    for (int y = 0; y < out.height; ++y)
    {
        for (int x = 0; x < out.width; ++x)
        {
            const int sx = 2 * x, sy = 2 * y;
            float sum = 0.0f;
            const float k[3] = {0.25f, 0.5f, 0.25f};
            for (int j = -1; j <= 1; ++j)
            {
                for (int i = -1; i <= 1; ++i)
                {
                    sum += k[i + 1] * k[j + 1] * p_In.at(sx + i, sy + j);
                }
            }
            out.data[size_t(y) * out.width + x] = sum;
        }
    }
    return out;
}

// Sum over a (2r + 1) square window around each pixel, edges clamped.
void BoxSum(std::vector<float>& p_Data, int p_Width, int p_Height, int p_Radius)
{
    std::vector<float> tmp(p_Data.size());
    for (int y = 0; y < p_Height; ++y)
    {
        const float* in = &p_Data[size_t(y) * p_Width];
        float* out = &tmp[size_t(y) * p_Width];
        float sum = 0.0f;
        for (int i = -p_Radius; i <= p_Radius; ++i)
        {
            sum += in[std::min(std::max(i, 0), p_Width - 1)];
        }
        for (int x = 0; x < p_Width; ++x)
        {
            out[x] = sum;
            sum += in[std::min(x + p_Radius + 1, p_Width - 1)] - in[std::max(x - p_Radius, 0)];
        }
    }
    for (int x = 0; x < p_Width; ++x)
    {
        float sum = 0.0f;
        for (int i = -p_Radius; i <= p_Radius; ++i)
        {
            sum += tmp[size_t(std::min(std::max(i, 0), p_Height - 1)) * p_Width + x];
        }
        for (int y = 0; y < p_Height; ++y)
        {
            p_Data[size_t(y) * p_Width + x] = sum;
            sum += tmp[size_t(std::min(y + p_Radius + 1, p_Height - 1)) * p_Width + x] - tmp[size_t(std::max(y - p_Radius, 0)) * p_Width + x];
        }
    }
}

// One pyramid level of Lucas-Kanade: refines p_U, p_V (pixels of this level) so that
// p_To(x + u, y + v) matches p_From(x, y). p_Confidence gets how well each window pins the
// movement down: the smaller eigenvalue of its structure tensor (0 in flat areas and along
// straight edges, where the movement cannot be measured).
void RefineLevel(const Plane& p_From, const Plane& p_To, std::vector<float>& p_U, std::vector<float>& p_V, std::vector<float>& p_Confidence)
{
    const int w = p_From.width, h = p_From.height;
    const size_t n = size_t(w) * h;
    // Gradients of the first frame stay fixed across the warps (inverse compositional style).
    std::vector<float> ix(n), iy(n);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            ix[size_t(y) * w + x] = 0.5f * (p_From.at(x + 1, y) - p_From.at(x - 1, y));
            iy[size_t(y) * w + x] = 0.5f * (p_From.at(x, y + 1) - p_From.at(x, y - 1));
        }
    }
    // Only pixels whose match lies inside the other picture count; at the edges the picture
    // there is unknown, and clamped edge pixels would pull the movement outwards.
    std::vector<float> a11(n), a12(n), a22(n), b1(n), b2(n);
    p_Confidence.assign(n, 0.0f);
    for (int iteration = 0; iteration < kIterations; ++iteration)
    {
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const size_t i = size_t(y) * w + x;
                const float tx = float(x) + p_U[i], ty = float(y) + p_V[i];
                const bool inside = tx >= 0.0f && ty >= 0.0f && tx <= float(w - 1) && ty <= float(h - 1);
                const float gx = inside ? ix[i] : 0.0f, gy = inside ? iy[i] : 0.0f;
                const float it = inside ? p_To.bilinear(tx, ty) - p_From.data[i] : 0.0f;
                a11[i] = gx * gx;
                a12[i] = gx * gy;
                a22[i] = gy * gy;
                b1[i] = gx * it;
                b2[i] = gy * it;
            }
        }
        for (std::vector<float>* sums : {&a11, &a12, &a22, &b1, &b2})
        {
            BoxSum(*sums, w, h, kWindowRadius);
        }
        for (size_t i = 0; i < n; ++i)
        {
            const float m11 = a11[i], m22 = a22[i], m12 = a12[i];
            const float mean = 0.5f * (m11 + m22), d = 0.5f * (m11 - m22);
            p_Confidence[i] = std::max(0.0f, mean - std::sqrt(d * d + m12 * m12));
            const float det = m11 * m22 - m12 * m12;
            // Only where the window pins the movement down in both directions.
            if (p_Confidence[i] < kMinConfidence || det <= 1e-12f)
            {
                continue;
            }
            const float du = -(m22 * b1[i] - m12 * b2[i]) / det;
            const float dv = -(m11 * b2[i] - m12 * b1[i]) / det;
            p_U[i] += std::min(std::max(du, -2.0f), 2.0f);
            p_V[i] += std::min(std::max(dv, -2.0f), 2.0f);
        }
    }
}

// Smooths the movement, weighting each pixel by how well it was measured, so that flat areas
// and edges take the movement of the textured parts around them (normalised convolution).
void SmoothFlow(std::vector<float>& p_U, std::vector<float>& p_V, const std::vector<float>& p_Confidence, int p_Width, int p_Height)
{
    const size_t n = size_t(p_Width) * p_Height;
    std::vector<float> weight(n), wu(n), wv(n);
    for (size_t i = 0; i < n; ++i)
    {
        weight[i] = p_Confidence[i] / (p_Confidence[i] + kConfidenceHalf) + 1e-6f;
        wu[i] = weight[i] * p_U[i];
        wv[i] = weight[i] * p_V[i];
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        BoxSum(weight, p_Width, p_Height, kSmoothRadius);
        BoxSum(wu, p_Width, p_Height, kSmoothRadius);
        BoxSum(wv, p_Width, p_Height, kSmoothRadius);
    }
    for (size_t i = 0; i < n; ++i)
    {
        p_U[i] = wu[i] / weight[i];
        p_V[i] = wv[i] / weight[i];
    }
}

// The small stitched frame as 8-bit BGRA.
std::vector<unsigned char> ToBGRA(const std::vector<float>& p_Rgba, int p_Width, int p_Height)
{
    std::vector<unsigned char> out(size_t(p_Width) * p_Height * 4);
    for (size_t i = 0; i < size_t(p_Width) * p_Height; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            out[i * 4 + c] = (unsigned char)(std::min(std::max(p_Rgba[i * 4 + 2 - c], 0.0f), 1.0f) * 255.0f + 0.5f);
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}
}

void MotionBlurSize(int p_Width, int p_Height, int& p_SmallWidth, int& p_SmallHeight)
{
    const double scale = std::min(1.0, double(kFlowSize) / std::max(p_Width, p_Height));
    p_SmallWidth = std::max(16, int(std::lround(p_Width * scale / 2.0)) * 2);
    p_SmallHeight = std::max(16, int(std::lround(p_Height * scale / 2.0)) * 2);
}

bool OpticalFlow(const unsigned char* p_From, const unsigned char* p_To, int p_Width, int p_Height,
                 std::vector<float>& p_Flow, int& p_FlowWidth, int& p_FlowHeight)
{
    if (!p_From || !p_To || p_Width < 2 || p_Height < 2)
    {
        return false;
    }
    std::vector<Plane> from = {Gray(p_From, p_Width, p_Height)}, to = {Gray(p_To, p_Width, p_Height)};
    while (std::min(from.back().width, from.back().height) / 2 >= kMinLevelSize)
    {
        from.push_back(Down(from.back()));
        to.push_back(Down(to.back()));
    }

    std::vector<float> u, v;
    for (int level = int(from.size()) - 1; level >= 0; --level)
    {
        const int w = from[level].width, h = from[level].height;
        std::vector<float> nu(size_t(w) * h, 0.0f), nv(size_t(w) * h, 0.0f);
        if (!u.empty())
        {
            // The coarser level's result, twice as long at this size.
            const int pw = from[level + 1].width, ph = from[level + 1].height;
            Plane cu{pw, ph, u}, cv{pw, ph, v};
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const float sx = (float(x) + 0.5f) * pw / w - 0.5f, sy = (float(y) + 0.5f) * ph / h - 0.5f;
                    nu[size_t(y) * w + x] = 2.0f * cu.bilinear(sx, sy);
                    nv[size_t(y) * w + x] = 2.0f * cv.bilinear(sx, sy);
                }
            }
        }
        std::vector<float> confidence;
        RefineLevel(from[level], to[level], nu, nv, confidence);
        SmoothFlow(nu, nv, confidence, w, h);
        u.swap(nu);
        v.swap(nv);
    }

    p_FlowWidth = p_Width;
    p_FlowHeight = p_Height;
    p_Flow.resize(size_t(p_Width) * p_Height * 2);
    const float longest = kMaxMotion * std::sqrt(float(p_Width) * p_Width + float(p_Height) * p_Height);
    for (size_t i = 0; i < size_t(p_Width) * p_Height; ++i)
    {
        const float length = std::sqrt(u[i] * u[i] + v[i] * v[i]);
        const float scale = length > longest ? longest / length : 1.0f;
        p_Flow[i * 2] = u[i] * scale / float(p_Width);
        p_Flow[i * 2 + 1] = v[i] * scale / float(p_Height);
    }
    return true;
}

bool RunMotionBlur(int p_Width, int p_Height, bool p_Wrap, float p_Amount, const StitchParams p_Params[3], const LensImages p_Lenses[3],
                   const bool p_Have[3], const FrameBuffer& p_Input, const FrameOutput& p_Output)
{
    if (!p_Have[1] || (!p_Have[0] && !p_Have[2]))
    {
        return false;
    }
    const int w = p_Params[1].width, h = p_Params[1].height;

    // The three small frames.
    std::vector<unsigned char> pictures[3];
    for (int i = 0; i < 3; ++i)
    {
        if (!p_Have[i])
        {
            continue;
        }
        std::vector<float> small(size_t(w) * h * 4);
        FrameOutput out;
        out.data = reinterpret_cast<unsigned char*>(small.data());
        out.width = w;
        out.height = h;
        out.rowBytes = long(w) * 16;
        if (!RunStitchKernel(p_Params[i], p_Input, out, &p_Lenses[i]))
        {
            return false;
        }
        pictures[i] = ToBGRA(small, w, h);
    }

    // Towards the previous and the next frame, in parallel; where there is no such frame, nothing moves.
    FlowField flows[2];
    bool ok[2] = {true, true};
    const bool have[2] = {p_Have[0], p_Have[2]};
    auto measure = [&](int i) {
        if (have[i])
        {
            ok[i] = OpticalFlow(pictures[1].data(), pictures[i == 0 ? 0 : 2].data(), w, h, flows[i].xy, flows[i].width, flows[i].height);
        }
        else
        {
            flows[i].xy.assign(2, 0.0f);
            flows[i].width = flows[i].height = 1;
        }
    };
    std::thread other(measure, 1);
    measure(0);
    other.join();
    if (!ok[0] || !ok[1])
    {
        return false;
    }
    return RunBlurKernel(p_Width, p_Height, p_Wrap, p_Amount, flows[0], flows[1], p_Output);
}
