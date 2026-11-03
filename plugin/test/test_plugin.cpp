// Tests for the Windows port, without a host application. Port of test_plugin.mm.
//
// Without options it loads the built plugin binary the way a host would and runs the kernel on
// test images:
//   test_plugin path/to/Insta360Stitch.ofx
//
// With --stitch it runs the stitch on a raw float RGBA frame instead, so that
// tools/compare_kernel.py can check the result against the Python reference:
//   test_plugin --stitch in.raw width height calibration.json|- out.raw flipY [pan tilt roll fov distance aspect [stab]]
//
// With --fullres it renders a frame of two lens files both ways, from the files decoded at full
// resolution and from a Resolve-like input (the same frame scaled to the timeline, decoded
// separately by counting frames from the start), and reports how much the colours differ, how
// much sharper full resolution is, and how long frames take:
//   test_plugin --fullres lensA.mov lensB.mov frame width height calibration.json|- out_prefix [pan tilt roll fov distance aspect]
//
// With --track it runs the subject tracker over numbered raw frames (frame_0000.raw, ...), or
// 8-bit RGB frames on stdin when dir is "-", and prints one line per frame: index pan tilt confidence
//   test_plugin --track dir width height count calibration.json|- pan tilt subjectDegrees
//
// With --blur it renders a stabilised frame with motion blur (amount 1 is the plugin's Motion Blur 100):
//   test_plugin --blur lensA.mov lensB.mov file.motion|- calibration.json|- frame width height amount out.raw [pan tilt roll fov distance aspect]
//
// With --flow it checks the optical flow and the motion file reader.
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "ofxCore.h"
#include "Calibration.h"
#include "LensDecoder.h"
#include "Motion.h"
#include "MotionBlur.h"
#include "Platform.h"
#include "StitchKernel.h"
#include "Tracker.h"

static int s_Failures = 0;

static void Check(bool p_Ok, const char* p_What)
{
    printf("%s  %s\n", p_Ok ? "ok  " : "FAIL", p_What);
    s_Failures += p_Ok ? 0 : 1;
}

static double Seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool RunKernelWith(const StitchParams& p_Params, const std::vector<float>& p_In, std::vector<float>& p_Out, const LensImages* p_Lenses,
                          std::shared_ptr<FullResCheck> p_Check = nullptr)
{
    p_Out.assign(p_In.size(), -1.0f);
    FrameBuffer src;
    src.data = reinterpret_cast<const unsigned char*>(p_In.data());
    src.width = p_Params.width;
    src.height = p_Params.height;
    src.rowBytes = long(p_Params.width) * 16;
    FrameOutput dst;
    dst.data = reinterpret_cast<unsigned char*>(p_Out.data());
    dst.width = p_Params.width;
    dst.height = p_Params.height;
    dst.rowBytes = long(p_Params.width) * 16;
    return RunStitchKernel(p_Params, src, dst, p_Lenses, p_Check);
}

static bool RunKernel(const StitchParams& p_Params, const std::vector<float>& p_In, std::vector<float>& p_Out)
{
    return RunKernelWith(p_Params, p_In, p_Out, nullptr);
}

static bool LoadCalibration(const char* p_Path, LensCalibration p_Lenses[2])
{
    std::string raw = kNominalX5Calibration;
    if (strcmp(p_Path, "-") != 0 && !ReadCalibrationFile(p_Path, raw))
    {
        fprintf(stderr, "cannot read calibration from %s\n", p_Path);
        return false;
    }
    if (!ParseCalibration(raw, p_Lenses))
    {
        fprintf(stderr, "calibration is not a 2-lens calibration\n");
        return false;
    }
    return true;
}

static int StitchFile(int argc, char** argv)
{
    // An optional 15th argument holds a stabilisation rotation: 9 comma-separated numbers.
    if (argc != 8 && argc != 14 && argc != 15)
    {
        fprintf(stderr, "usage: test_plugin --stitch in.raw width height calibration.json|- out.raw flipY [pan tilt roll fov distance aspect [stab]]\n");
        return 2;
    }

    StitchParams params = {};
    params.width = atoi(argv[3]);
    params.height = atoi(argv[4]);
    params.mode = eModeStitch;
    params.flipY = atoi(argv[7]);
    params.blendBand = float(sin(2.0 * M_PI / 180.0));
    params.gainMatch = 1;
    params.autoDistance = 1;
    if (argc >= 14)
    {
        params.mode = eModeReframe;
        FillViewParams(atof(argv[8]), atof(argv[9]), atof(argv[10]), atof(argv[11]), atof(argv[12]), atof(argv[13]), params);
    }
    if (argc == 15)
    {
        params.stabilize = 1;
        const char* p = argv[14];
        for (int i = 0; i < 9; ++i)
        {
            params.stab[i] = float(strtod(p, const_cast<char**>(&p)));
            p += (*p == ',') ? 1 : 0;
        }
    }
    LensCalibration lenses[2];
    if (!LoadCalibration(argv[5], lenses))
    {
        return 1;
    }
    FillLensParams(lenses, params.width, params.height, params.lens);
    FillLensOffset(lenses, params);

    std::vector<float> in(size_t(params.width) * params.height * 4), out;
    FILE* f = OpenFile(argv[2], "rb");
    if (!f || fread(in.data(), sizeof(float), in.size(), f) != in.size())
    {
        fprintf(stderr, "cannot read %zu floats from %s\n", in.size(), argv[2]);
        return 1;
    }
    fclose(f);
    if (!RunKernel(params, in, out))
    {
        return 1;
    }
    f = OpenFile(argv[6], "wb");
    if (!f || fwrite(out.data(), sizeof(float), out.size(), f) != out.size())
    {
        fprintf(stderr, "cannot write %s\n", argv[6]);
        return 1;
    }
    fclose(f);
    return 0;
}

// Frame p_Frame of a lens file as 8-bit BGRA scaled to p_W x p_H, decoded on its own (counting
// frames from the start, without seeking) so that it checks the lens decoder's frame numbers.
static bool DecodeScaled(const char* p_Path, int p_Frame, int p_W, int p_H, std::vector<uint8_t>& p_Out)
{
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, p_Path, nullptr, nullptr) != 0 || avformat_find_stream_info(format, nullptr) < 0)
    {
        return false;
    }
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* decoder = stream >= 0 ? avcodec_find_decoder(format->streams[stream]->codecpar->codec_id) : nullptr;
    AVCodecContext* codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    bool ok = false;
    if (codec && avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) >= 0 && avcodec_open2(codec, decoder, nullptr) == 0)
    {
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        int index = 0;
        bool ended = false;
        while (!ok)
        {
            const int got = avcodec_receive_frame(codec, frame);
            if (got == 0)
            {
                if (index++ == p_Frame)
                {
                    p_Out.resize(size_t(p_W) * p_H * 4);
                    SwsContext* scale = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), p_W, p_H, AV_PIX_FMT_BGRA,
                                                       SWS_AREA | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr);
                    // As the file says: BT.709, and full range unless marked as limited (the X5 records full range).
                    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
                    const int fullIn = frame->color_range == AVCOL_RANGE_MPEG ? 0 : 1;
                    if (scale)
                    {
                        sws_setColorspaceDetails(scale, bt709, fullIn, bt709, 1, 0, 1 << 16, 1 << 16);
                    }
                    uint8_t* planes[1] = {p_Out.data()};
                    const int strides[1] = {p_W * 4};
                    ok = scale && sws_scale(scale, frame->data, frame->linesize, 0, frame->height, planes, strides) == p_H;
                    sws_freeContext(scale);
                }
                av_frame_unref(frame);
                continue;
            }
            if (got != AVERROR(EAGAIN) || ended)
            {
                break;
            }
            int read;
            while ((read = av_read_frame(format, packet)) == 0 && packet->stream_index != stream)
            {
                av_packet_unref(packet);
            }
            if (read < 0)
            {
                ended = true;
                avcodec_send_packet(codec, nullptr);
                continue;
            }
            avcodec_send_packet(codec, packet);
            av_packet_unref(packet);
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
    }
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    return ok;
}

static void SetUpFullRes(const LensImages& p_Images, const LensCalibration p_Lenses[2], StitchParams& p_Params)
{
    p_Params.fullRes = 1;
    p_Params.lensWidth = p_Images.width;
    p_Params.lensHeight = p_Images.height;
    p_Params.seamLod = float(std::max(0.0, log2(p_Images.width / 960.0)));
    p_Params.yuvScale = 1.0f;
    p_Params.chromaMid = p_Images.bits > 8 ? float(512.0 / 1023.0) : float(128.0 / 255.0);
    FillLensParams(p_Lenses, 2 * p_Images.width, p_Images.height, p_Params.lens);
}

static int FullResFile(int argc, char** argv)
{
    if (argc != 9 && argc != 15)
    {
        fprintf(stderr, "usage: test_plugin --fullres lensA.mov lensB.mov frame width height calibration.json|- out_prefix [pan tilt roll fov distance aspect]\n");
        return 2;
    }
    const int frame = atoi(argv[4]);
    StitchParams params = {};
    params.width = atoi(argv[5]);
    params.height = atoi(argv[6]);
    params.mode = eModeStitch;
    params.flipY = 1;
    params.blendBand = float(sin(2.0 * M_PI / 180.0));
    params.gainMatch = 1;
    params.autoDistance = 1;
    if (argc == 15)
    {
        params.mode = eModeReframe;
        FillViewParams(atof(argv[9]), atof(argv[10]), atof(argv[11]), atof(argv[12]), atof(argv[13]), atof(argv[14]), params);
    }
    LensCalibration lenses[2];
    if (!LoadCalibration(argv[7], lenses))
    {
        return 1;
    }
    FillLensOffset(lenses, params);

    // Resolve-like input: each lens scaled to half the frame, side by side, bottom row first.
    const int tileW = params.width / 2;
    std::vector<float> in(size_t(params.width) * params.height * 4), outResolve, outFull;
    for (int lens = 0; lens < 2; ++lens)
    {
        std::vector<uint8_t> bgra;
        if (!DecodeScaled(argv[2 + lens], frame, tileW, params.height, bgra))
        {
            fprintf(stderr, "cannot decode frame %d of %s\n", frame, argv[2 + lens]);
            return 1;
        }
        for (int y = 0; y < params.height; ++y)
        {
            for (int x = 0; x < tileW; ++x)
            {
                const uint8_t* s = &bgra[(size_t(y) * tileW + x) * 4];
                float* d = &in[((size_t(params.height - 1 - y)) * params.width + lens * tileW + x) * 4];
                d[0] = s[2] / 255.0f;
                d[1] = s[1] / 255.0f;
                d[2] = s[0] / 255.0f;
                d[3] = 1.0f;
            }
        }
    }

    FillLensParams(lenses, params.width, params.height, params.lens);
    if (!RunKernel(params, in, outResolve))
    {
        return 1;
    }

    double start = Seconds();
    LensImages images;
    if (!AcquireLensImages(argv[2], argv[3], frame, images))
    {
        fprintf(stderr, "cannot decode the lens files at full resolution\n");
        return 1;
    }
    const double seek = Seconds() - start;
    StitchParams full = params;
    SetUpFullRes(images, lenses, full);
    auto check = std::make_shared<FullResCheck>();
    start = Seconds();
    RunKernelWith(full, in, outFull, &images, check);
    const double render = Seconds() - start;
    printf("frame check: %s, difference %.4f\n", check->matched == 1 ? "matched" : "NOT matched", double(check->lastDiff));
    s_Failures += check->matched == 1 ? 0 : 1;

    // A frame other than the input's must be noticed, and the input used instead.
    LensImages other;
    std::vector<float> outOther;
    auto otherCheck = std::make_shared<FullResCheck>();
    if (AcquireLensImages(argv[2], argv[3], frame + 45, other))
    {
        RunKernelWith(full, in, outOther, &other, otherCheck);
        double worst = 0.0, total = 0.0;
        for (size_t i = 0; i < outOther.size(); ++i)
        {
            worst = fmax(worst, fabs(outOther[i] - outResolve[i]));
            total += fabs(outOther[i] - outResolve[i]);
        }
        printf("frame 45 later: %s, difference %.4f; output vs input path: mean %.4f max %.4f (8-bit levels)\n",
               otherCheck->mismatched == 1 ? "rejected" : "NOT rejected", double(otherCheck->lastDiff), total / outOther.size() * 255, worst * 255);
        s_Failures += otherCheck->mismatched == 1 ? 0 : 1;
    }

    // Jumping back to the frame (a new seek) must give the same picture.
    LensImages again;
    std::vector<float> outAgain;
    if (AcquireLensImages(argv[2], argv[3], frame, again))
    {
        RunKernelWith(full, in, outAgain, &again);
        Check(outAgain == outFull, "seeking back to a frame decodes the same picture");
    }

    // Playing on: decode and render the next frames as a timeline would.
    const int count = 20;
    start = Seconds();
    for (int i = 1; i <= count; ++i)
    {
        LensImages next;
        std::vector<float> scratch;
        if (!AcquireLensImages(argv[2], argv[3], frame + i, next))
        {
            fprintf(stderr, "cannot decode frame %d\n", frame + i);
            return 1;
        }
        RunKernelWith(full, in, scratch, &next);
    }
    const double perFrame = (Seconds() - start) / count;
    printf("%s\n", LensDecoderStats().c_str());
    printf("lenses %dx%d %d-bit; first frame decode %.0f ms, render %.0f ms; then %.1f ms a frame (%.1f fps) at %dx%d\n", images.width,
           images.height, images.bits, seek * 1000, render * 1000, perFrame * 1000, 1.0 / perFrame, params.width, params.height);

    // Colour: compare 16 x 16 pixel block averages, which leaves out the difference in sharpness.
    const int block = 16, bw = params.width / block, bh = params.height / block;
    double diff[3] = {}, mean[2][3] = {}, detail[2] = {};
    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            double sums[2][3] = {};
            for (int y = by * block; y < (by + 1) * block; ++y)
            {
                for (int x = bx * block; x < (bx + 1) * block; ++x)
                {
                    for (int c = 0; c < 3; ++c)
                    {
                        sums[0][c] += outResolve[(size_t(y) * params.width + x) * 4 + c];
                        sums[1][c] += outFull[(size_t(y) * params.width + x) * 4 + c];
                    }
                }
            }
            for (int c = 0; c < 3; ++c)
            {
                diff[c] += fabs(sums[1][c] - sums[0][c]) / (block * block);
                mean[0][c] += sums[0][c] / (block * block);
                mean[1][c] += sums[1][c] / (block * block);
            }
        }
    }
    for (int y = 0; y < params.height; ++y)
    {
        for (int x = 1; x < params.width; ++x)
        {
            const size_t i = (size_t(y) * params.width + x) * 4 + 1;
            detail[0] += fabs(outResolve[i] - outResolve[i - 4]);
            detail[1] += fabs(outFull[i] - outFull[i - 4]);
        }
    }
    const double n = double(bw) * bh;
    printf("mean RGB (8-bit levels): Resolve-like %.2f %.2f %.2f, full resolution %.2f %.2f %.2f\n", mean[0][0] / n * 255, mean[0][1] / n * 255,
           mean[0][2] / n * 255, mean[1][0] / n * 255, mean[1][1] / n * 255, mean[1][2] / n * 255);
    printf("colour difference of 16x16 block averages (8-bit levels): %.2f %.2f %.2f\n", diff[0] / n * 255, diff[1] / n * 255, diff[2] / n * 255);
    printf("detail (neighbour difference), full resolution / Resolve-like: %.2f\n", detail[1] / fmax(detail[0], 1e-9));

    for (int k = 0; k < 2; ++k)
    {
        const std::string path = std::string(argv[8]) + (k ? "_full.raw" : "_resolve.raw");
        const std::vector<float>& out = k ? outFull : outResolve;
        FILE* f = OpenFile(path, "wb");
        if (!f)
        {
            return 1;
        }
        fwrite(out.data(), sizeof(float), out.size(), f);
        fclose(f);
    }
    return s_Failures == 0 ? 0 : 1;
}

static int TrackFiles(int argc, char** argv)
{
    if (argc != 10)
    {
        fprintf(stderr, "usage: test_plugin --track dir width height count calibration.json|- pan tilt subjectDegrees\n");
        return 2;
    }
    const int w = atoi(argv[3]), h = atoi(argv[4]), count = atoi(argv[5]);
    LensCalibration lenses[2];
    if (!LoadCalibration(argv[6], lenses))
    {
        return 1;
    }

    std::vector<float> pixels(size_t(w) * h * 4);
    std::vector<unsigned char> rgb(size_t(w) * h * 3);
    const bool fromStdin = strcmp(argv[2], "-") == 0;
    auto load = [&](int i) {
        if (fromStdin)
        {
            if (fread(rgb.data(), 1, rgb.size(), stdin) != rgb.size())
            {
                return false;
            }
            for (size_t p = 0; p < size_t(w) * h; ++p)
            {
                pixels[p * 4] = rgb[p * 3] / 255.0f;
                pixels[p * 4 + 1] = rgb[p * 3 + 1] / 255.0f;
                pixels[p * 4 + 2] = rgb[p * 3 + 2] / 255.0f;
                pixels[p * 4 + 3] = 1.0f;
            }
            return true;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame_%04d.raw", argv[2], i);
        FILE* f = OpenFile(path, "rb");
        const bool ok = f && fread(pixels.data(), sizeof(float), pixels.size(), f) == pixels.size();
        if (f)
        {
            fclose(f);
        }
        return ok;
    };
    const FrameView frame = {reinterpret_cast<const unsigned char*>(pixels.data()), w, h, long(w * 4 * sizeof(float)), false};

    double pan = atof(argv[7]), tilt = atof(argv[8]), confidence = 1.0;
    SubjectTracker tracker(lenses, atof(argv[9]));
    if (!load(0))
    {
        fprintf(stderr, "cannot read frame 0\n");
        return 1;
    }
    tracker.start(frame, pan, tilt);
    printf("0 %.3f %.3f 1.000\n", pan, tilt);
    for (int i = 1; i < count; ++i)
    {
        if (!load(i))
        {
            fprintf(stderr, "cannot read frame %d\n", i);
            return 1;
        }
        const bool going = tracker.step(frame, pan, tilt, confidence);
        printf("%d %.3f %.3f %.3f\n", i, pan, tilt, confidence);
        if (!going)
        {
            printf("lost\n");
            break;
        }
    }
    return 0;
}

// A whole stabilised frame with motion blur, as the plugin renders it.
static int BlurFile(int argc, char** argv)
{
    if (argc != 11 && argc != 17)
    {
        fprintf(stderr, "usage: test_plugin --blur lensA.mov lensB.mov file.motion|- calibration.json|- frame width height amount out.raw [pan tilt roll fov distance aspect]\n");
        return 2;
    }
    const auto motion = strcmp(argv[4], "-") != 0 ? MotionData::Load(argv[4]) : nullptr;
    LensCalibration lenses[2];
    if (!LoadCalibration(argv[5], lenses))
    {
        return 1;
    }
    const int frame = atoi(argv[6]), width = atoi(argv[7]), height = atoi(argv[8]);
    const float amount = float(atof(argv[9]));

    StitchParams base = {};
    base.mode = argc == 17 ? eModeReframe : eModeStitch;
    base.flipY = 1;
    base.blendBand = float(sin(2.0 * M_PI / 180.0));
    base.gainMatch = 1;
    base.autoDistance = 1;
    base.trustDecoded = 1;
    if (argc == 17)
    {
        FillViewParams(atof(argv[11]), atof(argv[12]), atof(argv[13]), atof(argv[14]), atof(argv[15]), atof(argv[16]), base);
    }
    FillLensOffset(lenses, base);
    const auto setUp = [&](int p_Frame, int p_W, int p_H, LensImages& p_Images, StitchParams& p_Out) {
        if (p_Frame < 0 || !AcquireLensImages(argv[2], argv[3], p_Frame, p_Images))
        {
            return false;
        }
        p_Out = base;
        p_Out.width = p_W;
        p_Out.height = p_H;
        SetUpFullRes(p_Images, lenses, p_Out);
        if (motion)
        {
            double r[9];
            motion->viewToCamera(p_Frame, 1, 1.0, true, r);
            p_Out.stabilize = 1;
            for (int i = 0; i < 9; ++i)
            {
                p_Out.stab[i] = float(r[i]);
            }
        }
        return true;
    };

    LensImages images;
    StitchParams params;
    if (!setUp(frame, width, height, images, params))
    {
        return 1;
    }
    std::vector<float> src(size_t(width) * height * 4, 0.0f), dst;
    RunKernelWith(params, src, dst, &images);

    double blurSeconds = 0.0;
    if (amount > 0.0f)
    {
        int smallW, smallH;
        MotionBlurSize(width, height, smallW, smallH);
        StitchParams frames[3];
        LensImages small[3];
        bool have[3];
        const double start = Seconds();
        for (int i = 0; i < 3; ++i)
        {
            have[i] = setUp(frame + i - 1, smallW, smallH, small[i], frames[i]);
        }
        FrameBuffer in;
        in.data = reinterpret_cast<const unsigned char*>(src.data());
        in.width = width;
        in.height = height;
        in.rowBytes = long(width) * 16;
        FrameOutput out;
        out.data = reinterpret_cast<unsigned char*>(dst.data());
        out.width = width;
        out.height = height;
        out.rowBytes = long(width) * 16;
        if (!RunMotionBlur(width, height, base.mode == eModeStitch, amount, frames, small, have, in, out))
        {
            fprintf(stderr, "motion blur failed\n");
            return 1;
        }
        blurSeconds = Seconds() - start;
    }
    FILE* f = OpenFile(argv[10], "wb");
    if (!f)
    {
        return 1;
    }
    fwrite(dst.data(), sizeof(float), dst.size(), f);
    fclose(f);
    printf("frame %d: blur added %.0f ms\n", frame, blurSeconds * 1000.0);
    return 0;
}

// A clip imported again while Resolve is open rewrites its motion file.
static void TestMotionReload()
{
    const std::string path = JoinPath(SupportDir().empty() ? std::string(".") : SupportDir(), "test_motion_reload.motion");
    MakeDirectories(SupportDir());
    const auto write = [&](float p_Speed, int p_Frames) {
        FILE* f = OpenFile(path, "wb");
        const uint32_t count = uint32_t(p_Frames);
        const float fps = 30.0f, frame[7] = {1, 0, 0, 0, 0, -1, 0};
        fwrite("I360MOT1", 1, 8, f);
        fwrite(&count, 4, 1, f);
        fwrite(&fps, 4, 1, f);
        for (int i = 0; i < p_Frames; ++i)
        {
            fwrite(frame, 4, 7, f);
        }
        if (p_Speed > 0.0f)
        {
            fwrite(&p_Speed, 4, 1, f);
        }
        fclose(f);
    };
    write(0.0f, 1);
    const auto before = MotionData::Load(path);
    const bool same = before && MotionData::Load(path) == before;
    write(10.0f, 2); // a different size: the change is noticed even within the same second
    const auto after = MotionData::Load(path);
    Check(same && before->speed() == 1.0 && after && after->speed() == 10.0f, "a rewritten motion file is read again");
    remove(path.c_str());
}

// Checks which way the flow points: moves a picture by a known amount.
static void TestOpticalFlow(int w, int h, int dx, int dy)
{
    std::vector<unsigned char> a(size_t(w) * h * 4), b(size_t(w) * h * 4);
    // Detail at many sizes that does not repeat, like a photograph.
    std::vector<float> noise(size_t(w + 64) * (h + 64));
    srand(7);
    for (int size : {4, 8, 16, 32})
    {
        std::vector<float> cells(size_t((w + 64) / size + 2) * ((h + 64) / size + 2));
        for (float& v : cells)
        {
            v = float(rand()) / RAND_MAX - 0.5f;
        }
        const int stride = (w + 64) / size + 2;
        for (int y = 0; y < h + 64; ++y)
        {
            for (int x = 0; x < w + 64; ++x)
            {
                const float fx = float(x) / size, fy = float(y) / size;
                const int ix = int(fx), iy = int(fy);
                const float tx = fx - ix, ty = fy - iy;
                noise[size_t(y) * (w + 64) + x] += 60.0f * ((1 - ty) * ((1 - tx) * cells[iy * stride + ix] + tx * cells[iy * stride + ix + 1]) +
                                                            ty * ((1 - tx) * cells[(iy + 1) * stride + ix] + tx * cells[(iy + 1) * stride + ix + 1]));
            }
        }
    }
    const auto texture = [&](int x, int y) {
        return (unsigned char)std::min(255.0f, std::max(0.0f, 127.0f + noise[size_t(y + 32) * (w + 64) + x + 32]));
    };
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            for (int c = 0; c < 3; ++c)
            {
                a[(size_t(y) * w + x) * 4 + c] = texture(x, y);
                b[(size_t(y) * w + x) * 4 + c] = texture(x - dx, y - dy); // the picture moved by +dx, +dy rows
            }
            a[(size_t(y) * w + x) * 4 + 3] = b[(size_t(y) * w + x) * 4 + 3] = 255;
        }
    }
    std::vector<float> flow;
    int fw = 0, fh = 0;
    const double start = Seconds();
    const bool ok = OpticalFlow(a.data(), b.data(), w, h, flow, fw, fh);
    const double took = Seconds() - start;
    double mx = 0.0, my = 0.0;
    if (ok)
    {
        // The median over the middle of the picture.
        std::vector<float> xs, ys;
        for (int y = fh / 4; y < 3 * fh / 4; ++y)
        {
            for (int x = fw / 4; x < 3 * fw / 4; ++x)
            {
                xs.push_back(flow[(size_t(y) * fw + x) * 2] * w);
                ys.push_back(flow[(size_t(y) * fw + x) * 2 + 1] * h);
            }
        }
        std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
        std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
        mx = xs[xs.size() / 2];
        my = ys[ys.size() / 2];
    }
    printf("     optical flow %dx%d: moved (%d, %d), measured (%.2f, %.2f) in %.0f ms\n", fw, fh, dx, dy, mx, my, took * 1000.0);
    Check(ok && fabs(mx - dx) < 1.5 && fabs(my - dy) < 1.5, "optical flow points from a frame to where its pixels go");
}

// Flat areas (sky, walls) cannot show movement: the flow there must not invent any, or the
// motion blur smears them. Half the picture is flat, the other half textured; all of it moves.
static void TestFlowFlatAreas(int w, int h, int dx, int dy)
{
    std::vector<unsigned char> a(size_t(w) * h * 4), b(size_t(w) * h * 4);
    srand(11);
    std::vector<float> cells(size_t(w / 8 + 4) * (h / 8 + 4));
    for (float& v : cells)
    {
        v = float(rand()) / RAND_MAX;
    }
    const int stride = w / 8 + 4;
    const auto texture = [&](int x, int y) {
        if (x < w / 2)
        {
            return (unsigned char)200; // flat, like a bright sky
        }
        const float fx = (x + 16) / 8.0f, fy = (y + 16) / 8.0f;
        const int ix = std::min(std::max(int(fx), 0), stride - 2), iy = std::min(std::max(int(fy), 0), h / 8 + 2);
        const float tx = fx - ix, ty = fy - iy;
        const float v = (1 - ty) * ((1 - tx) * cells[iy * stride + ix] + tx * cells[iy * stride + ix + 1]) +
                        ty * ((1 - tx) * cells[(iy + 1) * stride + ix] + tx * cells[(iy + 1) * stride + ix + 1]);
        return (unsigned char)(40 + 150 * v);
    };
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            for (int c = 0; c < 3; ++c)
            {
                a[(size_t(y) * w + x) * 4 + c] = texture(x, y);
                b[(size_t(y) * w + x) * 4 + c] = texture(std::min(std::max(x - dx, 0), w - 1), std::min(std::max(y - dy, 0), h - 1));
            }
            a[(size_t(y) * w + x) * 4 + 3] = b[(size_t(y) * w + x) * 4 + 3] = 255;
        }
    }
    std::vector<float> flow;
    int fw = 0, fh = 0;
    const bool ok = OpticalFlow(a.data(), b.data(), w, h, flow, fw, fh);
    double worst = 0.0;
    for (int y = 0; y < fh && ok; ++y)
    {
        for (int x = 0; x < fw; ++x)
        {
            const double u = flow[(size_t(y) * fw + x) * 2] * w, v = flow[(size_t(y) * fw + x) * 2 + 1] * h;
            worst = std::max(worst, std::sqrt(u * u + v * v));
        }
    }
    const double moved = std::sqrt(double(dx * dx + dy * dy));
    printf("     flat half: longest vector %.2f pixels, picture moved %.2f\n", worst, moved);
    // (Flat areas next to the moving texture pick up some of its movement; wild vectors, the
    // failure this guards against, are several times longer.)
    Check(ok && worst < moved + 4.0, "flat areas get no movement much larger than the picture's");
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // The arguments as UTF-8 (char** argv is in the ANSI code page), for paths with umlauts.
    int wideCount = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    static std::vector<std::string> utf8;
    static std::vector<char*> utf8Argv;
    if (wide && wideCount == argc)
    {
        for (int i = 0; i < wideCount; ++i)
        {
            utf8.push_back(Narrow(wide[i]));
        }
        for (std::string& a : utf8)
        {
            utf8Argv.push_back(&a[0]);
        }
        utf8Argv.push_back(nullptr);
        argv = utf8Argv.data();
    }
#endif
    if (argc >= 2 && strcmp(argv[1], "--flow") == 0)
    {
        TestMotionReload();
        TestOpticalFlow(640, 360, 12, -7);
        TestOpticalFlow(640, 320, -20, 4);
        TestOpticalFlow(360, 640, 5, 18);
        TestFlowFlatAreas(640, 360, 6, 3);
        TestFlowFlatAreas(640, 360, -15, 0);
        return s_Failures;
    }
    if (argc >= 2 && strcmp(argv[1], "--blur") == 0)
    {
        return BlurFile(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "--track") == 0)
    {
        return TrackFiles(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "--fullres") == 0)
    {
        return FullResFile(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "--stitch") == 0)
    {
        return StitchFile(argc, argv);
    }
    if (argc == 5 && strcmp(argv[1], "--tag") == 0)
    {
        // test_plugin --tag frame.raw width height: the clip number in a frame (float RGBA, top row first)
        StitchParams params = {};
        params.width = atoi(argv[3]);
        params.height = atoi(argv[4]);
        std::vector<float> in(size_t(params.width) * params.height * 4);
        FILE* f = OpenFile(argv[2], "rb");
        if (!f || fread(in.data(), sizeof(float), in.size(), f) != in.size())
        {
            return 1;
        }
        fclose(f);
        FrameBuffer src;
        src.data = reinterpret_cast<const unsigned char*>(in.data());
        src.width = params.width;
        src.height = params.height;
        src.rowBytes = long(params.width) * 16;
        printf("%06x\n", ReadClipTag(params, src) & 0xFFFFFF);
        return 0;
    }
    if (argc < 2)
    {
        fprintf(stderr, "usage: test_plugin path/to/Insta360Stitch.ofx\n");
        return 2;
    }

    typedef int (*CountFn)(void);
    typedef OfxPlugin* (*GetFn)(int);
#ifdef _WIN32
    HMODULE lib = LoadLibraryW(Widen(argv[1]).c_str());
    CountFn count = lib ? (CountFn)(void*)GetProcAddress(lib, "OfxGetNumberOfPlugins") : nullptr;
    GetFn get = lib ? (GetFn)(void*)GetProcAddress(lib, "OfxGetPlugin") : nullptr;
#else
    void* lib = dlopen(argv[1], RTLD_NOW);
    CountFn count = lib ? (CountFn)dlsym(lib, "OfxGetNumberOfPlugins") : nullptr;
    GetFn get = lib ? (GetFn)dlsym(lib, "OfxGetPlugin") : nullptr;
#endif
    Check(lib != nullptr, "plugin binary loads");
    Check(count && get, "OpenFX entry points exported");
    if (count && get)
    {
        Check(count() == 1, "exactly one plugin in the bundle");
        OfxPlugin* plugin = get(0);
        printf("      identifier %s, version %d.%d, api %s\n", plugin->pluginIdentifier, plugin->pluginVersionMajor, plugin->pluginVersionMinor,
               plugin->pluginApi);
        Check(std::string(plugin->pluginIdentifier) == "org.insta360resolve.Stitch", "plugin identifier");
    }

    const int w = 64, h = 32; // 2:1 like the real frame
    std::vector<float> in(w * h * 4), out;
    for (size_t i = 0; i < in.size(); ++i)
    {
        in[i] = float(i % 251) / 250.f;
    }

    StitchParams params = {};
    params.width = w;
    params.height = h;
    params.mode = eModeSource;
    Check(RunKernel(params, in, out), "kernel runs");
    Check(out == in, "source view is identical to input");

    params.mode = eModeSourceTint;
    RunKernel(params, in, out);
    bool tintOk = true;
    for (int y = 0; y < h && tintOk; ++y)
    {
        for (int x = 0; x < w && tintOk; ++x)
        {
            const int i = (y * w + x) * 4;
            const bool left = x < w / 2;
            const float dim = y < h / 8 ? .25f : 1.f;
            const float e[4] = {in[i] * (left ? 1.f : .5f) * dim, in[i + 1] * .5f * dim, in[i + 2] * (left ? .5f : 1.f) * dim, in[i + 3]};
            for (int c = 0; c < 4; ++c)
            {
                tintOk = tintOk && std::fabs(out[i + c] - e[c]) < 1e-6f;
            }
        }
    }
    Check(tintOk, "tinted view: left half red, right half blue, first rows dark, alpha untouched");

    // A render window: only that part of the output is written, from rows of any stride.
    {
        StitchParams source = params;
        source.mode = eModeSource;
        std::vector<float> padded(size_t(w + 8) * h * 4, 0.5f), target(size_t(w + 8) * h * 4, -1.0f);
        for (int y = 0; y < h; ++y)
        {
            memcpy(&padded[size_t(y) * (w + 8) * 4], &in[size_t(y) * w * 4], size_t(w) * 16);
        }
        FrameBuffer src;
        src.data = reinterpret_cast<const unsigned char*>(padded.data());
        src.width = w;
        src.height = h;
        src.rowBytes = long(w + 8) * 16;
        FrameOutput dst;
        dst.data = reinterpret_cast<unsigned char*>(target.data());
        dst.width = w;
        dst.height = h;
        dst.rowBytes = long(w + 8) * 16;
        KernelWindow window;
        window.x1 = 4;
        window.x2 = 20;
        window.y1 = 3;
        window.y2 = 9;
        RunStitchKernel(source, src, dst, nullptr, nullptr, window);
        bool windowOk = true;
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w + 8; ++x)
            {
                const bool inside = x >= 4 && x < 20 && y >= 3 && y < 9;
                const float got = target[(size_t(y) * (w + 8) + x) * 4];
                windowOk = windowOk && (inside ? got == in[(size_t(y) * w + x) * 4] : got == -1.0f);
            }
        }
        Check(windowOk, "only the render window is written, with the host's row stride");
    }

    LensCalibration lenses[2];
    Check(ParseCalibration(kNominalX5Calibration, lenses), "built-in calibration parses");
    Check(!ParseCalibration("2_1.0_2.0", lenses), "truncated calibration is rejected");

    // Lens B 10% brighter and a little warmer than lens A: with Match Lenses on, both sides of the
    // seam meet at the geometric mean; with it off the step stays.
    {
        const int sw = 512, sh = 256;
        std::vector<float> flat(size_t(sw) * sh * 4), stitched;
        const float a[3] = {0.40f, 0.40f, 0.40f}, b[3] = {0.46f, 0.44f, 0.42f};
        for (int y = 0; y < sh; ++y)
        {
            for (int x = 0; x < sw; ++x)
            {
                float* px = &flat[(size_t(y) * sw + x) * 4];
                const float* c = x < sw / 2 ? a : b;
                px[0] = c[0], px[1] = c[1], px[2] = c[2], px[3] = 1.f;
            }
        }
        StitchParams seam = {};
        seam.width = sw;
        seam.height = sh;
        seam.mode = eModeStitch;
        seam.blendBand = float(sin(2.0 * M_PI / 180.0));
        FillLensParams(lenses, sw, sh, seam.lens);
        FillLensOffset(lenses, seam);
        seam.autoDistance = 1;
        // Equirectangular columns at longitude +-80 (lens A side) and +-100 (lens B side), on the equator.
        auto at = [&](double p_Lon, int p_C) { return stitched[(size_t(sh / 2) * sw + int((p_Lon + 180.0) / 360.0 * sw)) * 4 + p_C]; };
        for (int match = 0; match < 2; ++match)
        {
            seam.gainMatch = match;
            RunKernel(seam, flat, stitched);
            float step = 0.f, offMean = 0.f;
            for (int c = 0; c < 3; ++c)
            {
                step = std::max(step, std::fabs(at(80.0, c) - at(100.0, c)));
                offMean = std::max(offMean, std::fabs(at(80.0, c) - std::sqrt(a[c] * b[c])));
            }
            if (match)
            {
                Check(step < 1e-3f && offMean < 1e-3f, "Match Lenses evens out a lens exposure difference at the seam");
            }
            else
            {
                Check(step > 0.015f, "without Match Lenses the exposure step stays");
            }
        }
    }

    // The clip tag: written the way tools/clip_tag.py lays it out, read back from the corners.
    {
        const int tw = 1024, th = 512;
        std::vector<float> frame(size_t(tw) * th * 4, 0.0f);
        const uint32_t id = 0x3a5c71;
        uint32_t value = 0;
        {
            int crc = 0;
            for (int shift = 16; shift >= 0; shift -= 8)
            {
                crc ^= (id >> shift) & 0xFF;
                for (int i = 0; i < 8; ++i)
                {
                    crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xFF : (crc << 1) & 0xFF;
                }
            }
            value = id | (uint32_t(crc ^ 0x5A) << 24);
        }
        for (int bit = 0; bit < 32; ++bit)
        {
            if (!((value >> bit) & 1))
            {
                continue;
            }
            const int block = bit / 16, cell = bit % 16, row = cell / 4, col = cell % 4;
            const double step = 0.11 / 4, top = block == 0 ? 0.02 : 1.0 - 0.02 - 0.11;
            for (int y = int((top + row * step) * th); y < int((top + (row + 1) * step) * th); ++y)
            {
                for (int x = int((0.02 + col * step) * tw / 2); x < int((0.02 + (col + 1) * step) * tw / 2); ++x)
                {
                    float* px = &frame[(size_t(th - 1 - y) * tw + x) * 4]; // bottom row first
                    px[0] = px[1] = px[2] = px[3] = 1.0f;
                }
            }
        }
        StitchParams tag = {};
        tag.width = tw;
        tag.height = th;
        tag.flipY = 1;
        FrameBuffer src;
        src.data = reinterpret_cast<const unsigned char*>(frame.data());
        src.width = tw;
        src.height = th;
        src.rowBytes = long(tw) * 16;
        Check(ReadClipTag(tag, src) == int(id), "the clip tag is read from the corners of the frame");
        std::fill(frame.begin(), frame.end(), 0.0f);
        Check(ReadClipTag(tag, src) == -1, "a frame without a tag has none");
    }

    // Mip levels: each level is the average of the one before.
    {
        MipChain chain;
        chain.channels = 2;
        chain.levels.resize(1);
        chain.levels[0].width = 6;
        chain.levels[0].height = 4;
        chain.levels[0].data.assign(6 * 4 * 2, 0);
        for (size_t i = 0; i < chain.levels[0].data.size(); ++i)
        {
            chain.levels[0].data[i] = uint16_t((i * 9973) % 65536);
        }
        BuildMipLevels(chain);
        bool mipsOk = chain.levels.size() == 3 && chain.levels[1].width == 3 && chain.levels[1].height == 2 && chain.levels[2].width == 1 &&
                      chain.levels[2].height == 1;
        if (mipsOk)
        {
            const auto& l0 = chain.levels[0].data;
            const unsigned expect = (unsigned(l0[(0 * 6 + 2) * 2 + 1]) + l0[(0 * 6 + 3) * 2 + 1] + l0[(1 * 6 + 2) * 2 + 1] + l0[(1 * 6 + 3) * 2 + 1] + 2) / 4;
            mipsOk = chain.levels[1].data[(0 * 3 + 1) * 2 + 1] == expect;
        }
        Check(mipsOk, "mip levels halve the size and average 2 x 2 samples");
    }

    // The tracker starts from a point of the view; the view centre must be where the camera looks.
    StitchParams view = {};
    FillViewParams(40.0, -25.0, 10.0, 120.0, 0.5, 16.0 / 9.0, view);
    double ray[3];
    ViewPointToRay(view, 0.5, 0.5, ray);
    const double len = sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    const double rayPan = atan2(ray[0], ray[2]) * 180.0 / M_PI, rayTilt = asin(-ray[1] / len) * 180.0 / M_PI;
    Check(fabs(rayPan - 40.0) < 1e-6 && fabs(rayTilt + 25.0) < 1e-6, "view centre ray points along pan and tilt");
    ViewPointToRay(view, 0.9, 0.5, ray);
    Check(atan2(ray[0], ray[2]) * 180.0 / M_PI > 40.0, "a point right of centre lies further right");

    // Paths with characters outside ASCII (a user folder with an umlaut).
    {
        const std::string dir = JoinPath(SupportDir().empty() ? std::string(".") : SupportDir(), "Pr\xC3\xBC" "fung \xE2\x82\xAC");
        Check(MakeDirectories(dir), "a folder with an umlaut and a euro sign is created");
        const std::string file = JoinPath(dir, "calibration.json");
        FILE* f = OpenFile(file, "wb");
        if (f)
        {
            fprintf(f, "{\"calibration_raw\": \"%s\"}", kNominalX5Calibration);
            fclose(f);
        }
        std::string raw;
        Check(ReadCalibrationFile(file, raw) && raw == kNominalX5Calibration, "a calibration file is read from such a folder");
#ifdef _WIN32
        _wremove(Widen(file).c_str());
        _wrmdir(Widen(dir).c_str());
#else
        remove(file.c_str());
        rmdir(dir.c_str());
#endif
    }

    printf("%s\n", s_Failures ? "FAILED" : "all checks passed");
    return s_Failures ? 1 : 0;
}
