#pragma once

#include <atomic>
#include <memory>
#include <vector>

enum StitchMode
{
    eModeStitch = 0,     // dual fisheye in, equirectangular out
    eModeSource = 1,     // pass-through
    eModeSourceTint = 2, // pass-through, lens A half red, lens B half blue, first buffer rows darkened
    eModeReframe = 3,    // dual fisheye in, flat view through a virtual camera out
};

// Reframe camera model, as in Insta360 Studio: the virtual camera sits "distance" behind the
// centre of the sphere. 0 is a linear lens (straight lines stay straight), 1 is stereographic
// (action-cam look, tiny planet when looking down with a very wide view).

// Per-lens values in StitchParams::lens, already scaled to the frame being rendered.
enum LensSlot
{
    eLensFx = 0, eLensFy, eLensCx, eLensCy, eLensXi,
    eLensK1, eLensK2, eLensK3, eLensK4, eLensP1, eLensP2,
    eLensS1, eLensS2, eLensS3, eLensS4,
    eLensRot,            // 9 values: rows of the rig-to-lens rotation
    eLensSlotCount = eLensRot + 9
};

// Size of the per-frame seam table; SEAM_BINS in CpuKernel.cpp.
const int kSeamBins = 64;

// Parameters for the stitch kernel (CpuKernel.cpp; the macOS original ran the same maths in Metal).
struct StitchParams
{
    int width;      // frame size in pixels; buffers are tightly packed RGBA float
    int height;
    int mode;       // StitchMode
    int flipY;      // 1 = buffer row 0 is the bottom of the picture (OpenFX convention)
    float blendBand; // sin of half the seam blend width
    float distance;  // camera distance behind the sphere centre, 0..1, reframe only
    float planeX;    // half extent of the virtual camera's image plane, reframe only
    float planeY;
    float camRot[9]; // rows of the camera-to-rig rotation, reframe only
    int gainMatch;   // 1 = even out exposure and colour differences between the lenses at the seam
    int autoDistance;  // 1 = find the stitch distance along the seam each frame
    float invDistance; // 1 / stitch distance in metres when autoDistance is 0; 0 = infinity
    float lensOffset[3]; // position of lens B relative to lens A in the rig frame, metres
    float lens[2 * eLensSlotCount];
    int stabilize;   // 1 = turn every ray by stab (stabilisation), in both 360 and reframe output
    float stab[9];   // rows of the stabilised-view-to-camera rotation
    int fullRes;     // 1 = sample the lens files decoded at full resolution (LensDecoder.h), not the input
    int lensWidth;   // size of one decoded lens picture in pixels, fullRes only; lens is scaled to it
    int lensHeight;
    float seamLod;   // mip level the seam analysis reads, fullRes only
    float yuvScale;  // decoded sample value * yuvScale = code value / (2^bits - 1); 1 for LensDecoder.cpp
    float chromaMid; // zero chroma as code value / (2^bits - 1)
    int trustDecoded; // 1 = use the decoded lenses without comparing them with the input, fullRes only
};

struct LensImages;

// How often the decoded lenses matched the input picture (the kernel checks on every frame and
// uses the input when they do not).
struct FullResCheck
{
    std::atomic<int> matched{0};
    std::atomic<int> mismatched{0};
    std::atomic<float> lastDiff{0.0f}; // mean brightness difference of the last check, 0..1
};

// A host image of RGBA float pixels. Row 0 is the first row in memory; with StitchParams::flipY
// that is the bottom of the picture (OpenFX). rowBytes may be larger than width * 16.
struct FrameBuffer
{
    const unsigned char* data = nullptr;
    int width = 0;
    int height = 0;
    long rowBytes = 0;
};

struct FrameOutput
{
    unsigned char* data = nullptr;
    int width = 0;
    int height = 0;
    long rowBytes = 0;
};

// Part of the output to fill, in buffer rows and columns: [x1, x2) x [y1, y2). Empty = all of it.
struct KernelWindow
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// Reads the clip tag (ClipTag.h) from the source frame; returns the clip number or -1.
int ReadClipTag(const StitchParams& p_Params, const FrameBuffer& p_Input);

// Stitches, reframes or passes through p_Input into p_Output on the CPU (all cores). With
// p_Params.fullRes the lenses are read from p_Lenses instead of p_Input, after checking that
// they show the same frame. Both images are p_Params.width x p_Params.height.
bool RunStitchKernel(const StitchParams& p_Params, const FrameBuffer& p_Input, const FrameOutput& p_Output,
                     const LensImages* p_Lenses = nullptr, std::shared_ptr<FullResCheck> p_Check = nullptr,
                     KernelWindow p_Window = KernelWindow());

// Movement of the picture as x, y per pixel in fractions of the picture size (MotionBlur.h).
struct FlowField
{
    std::vector<float> xy;
    int width = 1;
    int height = 1;
};

// Smears p_Output, in place, along the movement towards the previous and the next frame.
// p_Wrap: the left and right edges meet, as in a 360 picture.
bool RunBlurKernel(int p_Width, int p_Height, bool p_Wrap, float p_Amount, const FlowField& p_Previous, const FlowField& p_Next,
                   const FrameOutput& p_Output);
