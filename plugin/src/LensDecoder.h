#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Full resolution lens pictures, decoded from the clip's own lens files (the .mov files the
// import copied out of the .insv) instead of taken from Resolve, which only hands an effect
// as many pixels as the timeline has.
//
// Each lens is kept as it is recorded, 4:2:0 YCbCr, as a luma plane and an interleaved CbCr
// plane with mip levels (so that views showing the lens smaller than recorded filter properly).
// Samples are 16-bit, scaled so that 65535 is the largest code value of the file's bit depth.
struct MipLevel
{
    int width = 0;
    int height = 0;
    std::vector<uint16_t> data; // width * height * channels, row by row from the top
};

struct MipChain
{
    int channels = 1;
    std::vector<MipLevel> levels; // level 0 is full size; each further level is half the size
};

struct LensImages
{
    const MipChain* planes[4] = {}; // luma A, chroma A, luma B, chroma B
    int width = 0;                  // size of one lens picture in pixels
    int height = 0;
    int bits = 8;                   // bits per component in the file
    std::shared_ptr<void> owner;    // keeps the planes alive
};

// Frame p_Frame of both lens files, decoded (on the GPU's video decoder where Windows offers
// one) and ready to sample. Recently used frames come from a cache. Returns false if a file
// cannot be read or has no such frame. Safe to call from several render threads at once.
bool AcquireLensImages(const std::string& p_LensA, const std::string& p_LensB, int p_Frame, LensImages& p_Images);

// Where the time goes, for the log: decoder in use and average milliseconds per frame pair
// since the last call (decoding, building mip levels). Resets the averages.
std::string LensDecoderStats();

// Lets go of all decoders and cached frames (the host asked the effect to free memory).
void ReleaseLensDecoders();

// Builds levels 1.. of p_Chain from level 0 (2 x 2 box filter). Exposed for tests.
void BuildMipLevels(MipChain& p_Chain);
