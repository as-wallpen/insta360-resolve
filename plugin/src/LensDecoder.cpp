// Decodes the lens files with FFmpeg's HEVC decoder for full resolution rendering; see
// LensDecoder.h. On Windows the GPU's video decoder is used through Direct3D 11 (D3D11VA) when
// the driver offers it, with FFmpeg's software decoder as the fallback. This replaces the
// AVFoundation decoder of the macOS original.
//
// The X5 writes a key frame every 60 frames and no B-frames, so a reader that is already
// positioned reads on cheaply, while a jump costs a seek and decoding from the key frame before it.

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>

#include "LensDecoder.h"
#include "Parallel.h"

namespace
{
const int kKeepDecoded = 4;      // recent frames kept per lens, for renders that arrive out of order
const int kReadAheadLimit = 30;  // frames read on rather than seeking to the frame
const int kKeepFrames = 3;       // frames kept ready to sample, for re-renders of the same frame
const double kIdleSeconds = 10.0; // readers and frames unused this long are released
const double kSweepEvery = 5.0;

double SecondsNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One lens of one frame: level 0 as decoded, further mip levels built on first use.
struct DecodedLens
{
    MipChain luma;
    MipChain chroma;
    int bits = 8;
    std::once_flag mips;

    void buildMips()
    {
        std::call_once(mips, [this]() {
            BuildMipLevels(luma);
            BuildMipLevels(chroma);
        });
    }
};

// 10-bit code value to 16 bits by repeating its top bits: 0 -> 0, 1023 -> 65535.
inline uint16_t Expand10(unsigned p_Code)
{
    p_Code &= 1023u;
    return uint16_t((p_Code << 6) | (p_Code >> 4));
}

// Limited range ("video") code value, already scaled to 16 bits, to full range.
inline uint16_t FullRange(float p_Value, bool p_Chroma)
{
    const float lo = 16.0f * 257.0f, span = (p_Chroma ? 224.0f : 219.0f) * 257.0f;
    const float mid = 32767.5f;
    const float v = p_Chroma ? (p_Value - 128.0f * 257.0f) / span * 65535.0f + mid : (p_Value - lo) / span * 65535.0f;
    return uint16_t(std::min(std::max(v, 0.0f), 65535.0f) + 0.5f);
}

// Copies a decoded 4:2:0 frame into a luma plane and an interleaved CbCr plane of 16-bit samples.
std::shared_ptr<DecodedLens> Convert(const AVFrame* p_Frame)
{
    const AVPixelFormat format = AVPixelFormat(p_Frame->format);
    const int w = p_Frame->width, h = p_Frame->height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    int bits = 0;
    bool planar = false;
    switch (format)
    {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        bits = 8;
        planar = true;
        break;
    case AV_PIX_FMT_YUV420P10LE:
        bits = 10;
        planar = true;
        break;
    case AV_PIX_FMT_NV12:
        bits = 8;
        break;
    case AV_PIX_FMT_P010LE:
        bits = 10;
        break;
    default:
        return nullptr;
    }
    auto lens = std::make_shared<DecodedLens>();
    lens->bits = bits;
    lens->luma.channels = 1;
    lens->chroma.channels = 2;
    lens->luma.levels.resize(1);
    lens->chroma.levels.resize(1);
    MipLevel& y = lens->luma.levels[0];
    MipLevel& c = lens->chroma.levels[0];
    y.width = w;
    y.height = h;
    c.width = cw;
    c.height = ch;
    y.data.resize(size_t(w) * h);
    c.data.resize(size_t(cw) * ch * 2);
    // The X5 records full range; a file that says otherwise is expanded to full range.
    const bool limited = p_Frame->color_range == AVCOL_RANGE_MPEG && format != AV_PIX_FMT_YUVJ420P;

    ParallelFor(h, 64, [&](int row) {
        uint16_t* out = &y.data[size_t(row) * w];
        const uint8_t* in = p_Frame->data[0] + long(row) * p_Frame->linesize[0];
        for (int x = 0; x < w; ++x)
        {
            uint16_t v;
            if (bits == 8)
            {
                v = uint16_t(in[x] * 257u);
            }
            else if (format == AV_PIX_FMT_P010LE)
            {
                v = Expand10(reinterpret_cast<const uint16_t*>(in)[x] >> 6);
            }
            else
            {
                v = Expand10(reinterpret_cast<const uint16_t*>(in)[x]);
            }
            out[x] = limited ? FullRange(float(v), false) : v;
        }
    });
    ParallelFor(ch, 64, [&](int row) {
        uint16_t* out = &c.data[size_t(row) * cw * 2];
        for (int x = 0; x < cw; ++x)
        {
            uint16_t cb, cr;
            if (planar)
            {
                const uint8_t* u = p_Frame->data[1] + long(row) * p_Frame->linesize[1];
                const uint8_t* v = p_Frame->data[2] + long(row) * p_Frame->linesize[2];
                if (bits == 8)
                {
                    cb = uint16_t(u[x] * 257u);
                    cr = uint16_t(v[x] * 257u);
                }
                else
                {
                    cb = Expand10(reinterpret_cast<const uint16_t*>(u)[x]);
                    cr = Expand10(reinterpret_cast<const uint16_t*>(v)[x]);
                }
            }
            else
            {
                const uint8_t* uv = p_Frame->data[1] + long(row) * p_Frame->linesize[1];
                if (bits == 8)
                {
                    cb = uint16_t(uv[x * 2] * 257u);
                    cr = uint16_t(uv[x * 2 + 1] * 257u);
                }
                else
                {
                    cb = Expand10(reinterpret_cast<const uint16_t*>(uv)[x * 2] >> 6);
                    cr = Expand10(reinterpret_cast<const uint16_t*>(uv)[x * 2 + 1] >> 6);
                }
            }
            out[x * 2] = limited ? FullRange(float(cb), true) : cb;
            out[x * 2 + 1] = limited ? FullRange(float(cr), true) : cr;
        }
    });
    return lens;
}

// Set by the hardware decoder's format choice; read by the reader that opened it.
struct HardwareChoice
{
    AVPixelFormat format = AV_PIX_FMT_NONE;
    bool failed = false; // the GPU turned the stream down and FFmpeg decodes it in software
};

enum AVPixelFormat PickHardwareFormat(AVCodecContext* p_Context, const enum AVPixelFormat* p_Formats)
{
    HardwareChoice* choice = static_cast<HardwareChoice*>(p_Context->opaque);
    for (const enum AVPixelFormat* f = p_Formats; *f != AV_PIX_FMT_NONE; ++f)
    {
        if (choice && *f == choice->format)
        {
            return *f;
        }
    }
    // The GPU cannot decode this stream: FFmpeg's software decoder takes over, for now on one
    // thread; the reader notices and opens a multithreaded software decoder next time.
    if (choice)
    {
        choice->failed = true;
    }
    for (const enum AVPixelFormat* f = p_Formats; *f != AV_PIX_FMT_NONE; ++f)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*f);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        {
            return *f;
        }
    }
    return AV_PIX_FMT_NONE;
}

// Sequential reader for one lens file.
class LensReader
{
public:
    explicit LensReader(const std::string& p_Path) : m_Path(p_Path) {}
    ~LensReader() { close(); }

    // Frame p_Frame, or nullptr.
    std::shared_ptr<DecodedLens> frame(int p_Frame)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_LastUsed = SecondsNow();
        if (std::shared_ptr<DecodedLens> kept = keptFrame(p_Frame))
        {
            return kept;
        }
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!m_Codec || p_Frame < m_Next || p_Frame - m_Next > kReadAheadLimit)
            {
                if (!open(p_Frame))
                {
                    return nullptr;
                }
            }
            int result = kRead;
            while (m_Next <= p_Frame && result == kRead)
            {
                result = readNext(p_Frame);
            }
            if (result == kRead)
            {
                std::shared_ptr<DecodedLens> frame = keptFrame(p_Frame);
                if (m_Hardware && m_Choice.failed)
                {
                    // The GPU declined this file: from the next frame on, software on all cores.
                    m_UseHardware = false;
                    close();
                }
                return frame;
            }
            if (result == kEnd)
            {
                close(); // asked for a frame past the end of the file
                return nullptr;
            }
            // A decoder that fails on the GPU gets a second try in software.
            const bool retry = m_UseHardware && m_Hardware;
            close();
            if (!retry)
            {
                return nullptr;
            }
            m_UseHardware = false;
        }
        return nullptr;
    }

    // Lets go of the decoder after a while without use.
    void releaseIfIdle(double p_Now)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (p_Now - m_LastUsed > kIdleSeconds)
        {
            close();
        }
    }

private:
    std::shared_ptr<DecodedLens> keptFrame(int p_Frame)
    {
        for (const auto& kept : m_Kept)
        {
            if (kept.first == p_Frame)
            {
                return kept.second;
            }
        }
        return nullptr;
    }

    bool openCodec(const AVCodec* p_Decoder, const AVCodecParameters* p_Params, bool p_Hardware)
    {
        m_Codec = avcodec_alloc_context3(p_Decoder);
        if (!m_Codec || avcodec_parameters_to_context(m_Codec, p_Params) < 0)
        {
            return false;
        }
        m_Hardware = false;
        if (p_Hardware)
        {
#ifdef _WIN32
            const AVHWDeviceType types[] = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2};
#else
            const AVHWDeviceType types[] = {AV_HWDEVICE_TYPE_NONE};
#endif
            for (AVHWDeviceType type : types)
            {
                if (type == AV_HWDEVICE_TYPE_NONE)
                {
                    continue;
                }
                AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
                for (int i = 0;; ++i)
                {
                    const AVCodecHWConfig* config = avcodec_get_hw_config(p_Decoder, i);
                    if (!config)
                    {
                        break;
                    }
                    if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type)
                    {
                        hwFormat = config->pix_fmt;
                        break;
                    }
                }
                AVBufferRef* device = nullptr;
                if (hwFormat != AV_PIX_FMT_NONE && av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) == 0)
                {
                    m_Codec->hw_device_ctx = device;
                    m_Choice = HardwareChoice();
                    m_Choice.format = hwFormat;
                    m_Codec->opaque = &m_Choice;
                    m_Codec->get_format = PickHardwareFormat;
                    m_Hardware = true;
                    break;
                }
            }
        }
        m_Codec->thread_count = m_Hardware ? 1 : 0; // software decoding: all cores
        return avcodec_open2(m_Codec, p_Decoder, nullptr) == 0;
    }

    bool open(int p_Frame)
    {
        close();
        if (avformat_open_input(&m_Format, m_Path.c_str(), nullptr, nullptr) != 0)
        {
            m_Format = nullptr;
            return false;
        }
        if (avformat_find_stream_info(m_Format, nullptr) < 0)
        {
            return false;
        }
        m_Stream = av_find_best_stream(m_Format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (m_Stream < 0)
        {
            return false;
        }
        AVStream* stream = m_Format->streams[m_Stream];
        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder || !openCodec(decoder, stream->codecpar, m_UseHardware))
        {
            return false;
        }
        m_Packet = av_packet_alloc();
        m_Decoded = av_frame_alloc();
        m_Transfer = av_frame_alloc();
        if (!m_Packet || !m_Decoded || !m_Transfer)
        {
            return false;
        }

        // Frame numbers from time stamps: the first frame's time and one frame's duration.
        m_TimeBase = av_q2d(stream->time_base);
        AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
        m_FrameSeconds = rate.num > 0 ? av_q2d(av_inv_q(rate)) : 1.0 / 30.0;
        m_Start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;

        // Seek to the key frame at or before this one; the decoder reads on from there.
        if (p_Frame > 0)
        {
            const int64_t target = m_Start + int64_t(std::llround((p_Frame - 0.25) * m_FrameSeconds / m_TimeBase));
            av_seek_frame(m_Format, m_Stream, target, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(m_Codec);
        }
        m_Next = 0;        // corrected by the first time stamp read
        m_Positioned = false;
        m_Ended = false;
        return true;
    }

    int frameIndex(const AVFrame* p_Frame) const
    {
        int64_t pts = p_Frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
        {
            pts = p_Frame->pts;
        }
        if (pts == AV_NOPTS_VALUE)
        {
            return m_Next;
        }
        return int(std::llround(double(pts - m_Start) * m_TimeBase / m_FrameSeconds));
    }

    enum { kRead, kEnd, kError };

    // Decodes the next frame; keeps it if it is close enough to p_Wanted to be asked for.
    int readNext(int p_Wanted)
    {
        for (;;)
        {
            const int got = avcodec_receive_frame(m_Codec, m_Decoded);
            if (got == 0)
            {
                const int index = frameIndex(m_Decoded);
                m_Positioned = true;
                m_Next = index + 1;
                bool ok = true;
                if (index > p_Wanted - kKeepDecoded)
                {
                    const AVFrame* frame = m_Decoded;
                    if (m_Decoded->hw_frames_ctx)
                    {
                        av_frame_unref(m_Transfer);
                        ok = av_hwframe_transfer_data(m_Transfer, m_Decoded, 0) == 0;
                        if (ok)
                        {
                            m_Transfer->color_range = m_Decoded->color_range;
                        }
                        frame = m_Transfer;
                    }
                    std::shared_ptr<DecodedLens> lens = ok ? Convert(frame) : nullptr;
                    ok = lens != nullptr;
                    if (ok)
                    {
                        m_Kept.emplace_back(index, lens);
                        if (int(m_Kept.size()) > kKeepDecoded)
                        {
                            m_Kept.pop_front();
                        }
                    }
                }
                av_frame_unref(m_Decoded);
                return ok ? kRead : kError;
            }
            if (got == AVERROR_EOF || (got == AVERROR(EAGAIN) && m_Ended))
            {
                return kEnd;
            }
            if (got != AVERROR(EAGAIN))
            {
                return kError;
            }
            // The decoder needs more data: the next packet of the lens stream, or the end.
            int read;
            while ((read = av_read_frame(m_Format, m_Packet)) == 0 && m_Packet->stream_index != m_Stream)
            {
                av_packet_unref(m_Packet);
            }
            if (read < 0)
            {
                m_Ended = true;
                avcodec_send_packet(m_Codec, nullptr); // drain the frames still in the decoder
                continue;
            }
            const int sent = avcodec_send_packet(m_Codec, m_Packet);
            av_packet_unref(m_Packet);
            if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_INVALIDDATA)
            {
                return kError;
            }
        }
    }

    void close()
    {
        av_frame_free(&m_Decoded);
        av_frame_free(&m_Transfer);
        av_packet_free(&m_Packet);
        avcodec_free_context(&m_Codec);
        avformat_close_input(&m_Format);
        m_Kept.clear();
        m_Hardware = false;
    }

    std::string m_Path;
    std::mutex m_Mutex;
    AVFormatContext* m_Format = nullptr;
    AVCodecContext* m_Codec = nullptr;
    AVPacket* m_Packet = nullptr;
    AVFrame* m_Decoded = nullptr;
    AVFrame* m_Transfer = nullptr;
    int m_Stream = -1;
    double m_TimeBase = 1.0 / 90000.0;
    double m_FrameSeconds = 1.0 / 30.0;
    int64_t m_Start = 0;
    int m_Next = 0; // frame the decoder returns next
    bool m_Positioned = false;
    bool m_Ended = false;
    bool m_UseHardware = true; // cleared for good when the GPU decoder failed on this file
    bool m_Hardware = false;   // the open decoder runs on the GPU
    HardwareChoice m_Choice;
    std::deque<std::pair<int, std::shared_ptr<DecodedLens>>> m_Kept;
    double m_LastUsed = 0.0;
};

struct FramePair
{
    std::shared_ptr<DecodedLens> lens[2];
};

struct CachedFrame
{
    std::string clip;
    int frame;
    double lastUsed;
    std::shared_ptr<FramePair> pair;
};

std::mutex s_Mutex;
std::map<std::string, std::shared_ptr<LensReader>> s_Readers; // by file path
std::list<CachedFrame> s_Frames;                              // most recently used first
double s_LastSweep = 0.0;

std::shared_ptr<LensReader> ReaderFor(const std::string& p_Path)
{
    std::shared_ptr<LensReader>& reader = s_Readers[p_Path];
    if (!reader)
    {
        reader = std::make_shared<LensReader>(p_Path);
    }
    return reader;
}

// Releases idle readers and cached frames. Runs from AcquireLensImages every few seconds:
// a DLL must not keep a thread of its own running (it could not stop it when it is unloaded).
void SweepIdle()
{
    const double now = SecondsNow();
    std::map<std::string, std::shared_ptr<LensReader>> readers;
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        if (now - s_LastSweep < kSweepEvery)
        {
            return;
        }
        s_LastSweep = now;
        readers = s_Readers;
        s_Frames.remove_if([now](const CachedFrame& p_Frame) { return now - p_Frame.lastUsed > kIdleSeconds; });
    }
    for (auto& reader : readers)
    {
        reader.second->releaseIfIdle(now);
    }
}
}

void BuildMipLevels(MipChain& p_Chain)
{
    if (p_Chain.levels.empty())
    {
        return;
    }
    p_Chain.levels.resize(1);
    const int ch = p_Chain.channels;
    while (p_Chain.levels.back().width > 1 || p_Chain.levels.back().height > 1)
    {
        const MipLevel& src = p_Chain.levels.back();
        MipLevel dst;
        dst.width = std::max(1, src.width / 2);
        dst.height = std::max(1, src.height / 2);
        dst.data.resize(size_t(dst.width) * dst.height * ch);
        ParallelFor(dst.height, 32, [&](int y) {
            const int y0 = std::min(2 * y, src.height - 1), y1 = std::min(2 * y + 1, src.height - 1);
            for (int x = 0; x < dst.width; ++x)
            {
                const int x0 = std::min(2 * x, src.width - 1), x1 = std::min(2 * x + 1, src.width - 1);
                for (int c = 0; c < ch; ++c)
                {
                    const unsigned sum = unsigned(src.data[(size_t(y0) * src.width + x0) * ch + c]) + src.data[(size_t(y0) * src.width + x1) * ch + c] +
                                         src.data[(size_t(y1) * src.width + x0) * ch + c] + src.data[(size_t(y1) * src.width + x1) * ch + c];
                    dst.data[(size_t(y) * dst.width + x) * ch + c] = uint16_t((sum + 2) / 4);
                }
            }
        });
        p_Chain.levels.push_back(std::move(dst));
    }
}

bool AcquireLensImages(const std::string& p_LensA, const std::string& p_LensB, int p_Frame, LensImages& p_Images)
{
    if (p_Frame < 0 || p_LensA.empty() || p_LensB.empty())
    {
        return false;
    }
    SweepIdle();
    const std::string clip = p_LensA + "\n" + p_LensB;

    std::shared_ptr<FramePair> pair;
    std::shared_ptr<LensReader> readers[2];
    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        for (auto it = s_Frames.begin(); it != s_Frames.end(); ++it)
        {
            if (it->frame == p_Frame && it->clip == clip)
            {
                it->lastUsed = SecondsNow();
                pair = it->pair;
                s_Frames.splice(s_Frames.begin(), s_Frames, it);
                break;
            }
        }
        readers[0] = ReaderFor(p_LensA);
        readers[1] = ReaderFor(p_LensB);
    }

    if (!pair)
    {
        // Both lenses at once; each reader has its own lock, so concurrent renders of other
        // frames wait only for the lens they need.
        pair = std::make_shared<FramePair>();
        std::thread other([&]() { pair->lens[1] = readers[1]->frame(p_Frame); });
        pair->lens[0] = readers[0]->frame(p_Frame);
        other.join();
        if (!pair->lens[0] || !pair->lens[1])
        {
            return false;
        }
        const MipLevel& a = pair->lens[0]->luma.levels[0];
        const MipLevel& b = pair->lens[1]->luma.levels[0];
        if (a.width != b.width || a.height != b.height || pair->lens[0]->bits != pair->lens[1]->bits)
        {
            return false;
        }
        std::thread mips([&]() { pair->lens[1]->buildMips(); });
        pair->lens[0]->buildMips();
        mips.join();

        std::lock_guard<std::mutex> lock(s_Mutex);
        s_Frames.push_front(CachedFrame{clip, p_Frame, SecondsNow(), pair});
        while (int(s_Frames.size()) > kKeepFrames)
        {
            s_Frames.pop_back();
        }
    }

    for (int i = 0; i < 2; ++i)
    {
        p_Images.planes[i * 2] = &pair->lens[i]->luma;
        p_Images.planes[i * 2 + 1] = &pair->lens[i]->chroma;
    }
    p_Images.width = pair->lens[0]->luma.levels[0].width;
    p_Images.height = pair->lens[0]->luma.levels[0].height;
    p_Images.bits = pair->lens[0]->bits;
    p_Images.owner = pair;
    return true;
}

void ReleaseLensDecoders()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Frames.clear();
    s_Readers.clear(); // readers in use elsewhere live on until that render is done
}
