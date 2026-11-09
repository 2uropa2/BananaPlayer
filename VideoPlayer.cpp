#include "FileIO.hpp"
#include "VideoPlayer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

static void bpTrace(const char* fmt, ...) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    FILE* f = FileIO::open("C:\\work\\Player\\bp_trace.log", "a");
    if (!f) return;
    va_list a; va_start(a, fmt); std::vfprintf(f, fmt, a); va_end(a);
    std::fputc('\n', f); std::fclose(f);
}

// Probe trace: enabled only when BP_PROBE_TRACE=1 in the environment.
// Emits high-signal, low-frequency events (loadFile begin/end, first
// video frame delivered, natural end, unload) to bp_probe.log next to
// the exe so external test harnesses can correlate per-file outcomes
// without wading through the [push] noise.
static bool bpProbeEnabled() {
    static int cached = -1;
    if (cached == -1) {
        const char* v = std::getenv("BP_PROBE_TRACE");
        cached = (v && v[0] == '1') ? 1 : 0;
    }
    return cached != 0;
}
static void bpProbe(const char* fmt, ...) {
    if (!bpProbeEnabled()) return;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lk(mtx);
    FILE* f = FileIO::open("C:\\work\\Player\\bp_probe.log", "a");
    if (!f) return;
    std::fprintf(f, "%llu ", (unsigned long long)SDL_GetTicks());
    va_list a; va_start(a, fmt); std::vfprintf(f, fmt, a); va_end(a);
    std::fputc('\n', f); std::fclose(f);
}

static double ptsToSec(int64_t pts, AVRational tb) {
    if (pts == AV_NOPTS_VALUE) return 0.0;
    return pts * av_q2d(tb);
}

// ---------- HDR tone-map helpers ----------
//
// Everything below is straight ITU BT.2100 / BT.2020 / BT.709 with a Hable
// filmic curve for range compression. The YUV->RGB stage is done by hand
// (not via sws_setColorspaceDetails + sws_getCoefficients(SWS_CS_BT2020),
// which returns garbage / a fallback matrix on some FFmpeg builds and was
// the source of the "everything blue" symptom on the previous attempt).

// SMPTE ST 2084 (PQ) inverse EOTF: [0,1] -> linear light in nits (max 10000).
static double pqEotf(double e) {
    const double m1 = 2610.0 / 16384.0;
    const double m2 = 128.0 * 2523.0 / 4096.0;
    const double c1 = 3424.0 / 4096.0;
    const double c2 = 32.0 * 2413.0 / 4096.0;
    const double c3 = 32.0 * 2392.0 / 4096.0;
    if (e <= 0.0) return 0.0;
    double ep = std::pow(e, 1.0 / m2);
    double num = std::max(ep - c1, 0.0);
    double den = c2 - c3 * ep;
    if (den <= 0.0) return 0.0;
    return 10000.0 * std::pow(num / den, 1.0 / m1);
}

// ARIB STD-B67 (HLG) inverse OETF: [0,1] -> scene-referred [0,1] (the "12"
// scene-light reference of BT.2100 divided by 12). OOTF is applied later
// in the pixel loop because it needs the pixel's own luminance.
static double hlgInvOetf(double e) {
    const double a = 0.17883277;
    const double b = 0.28466892;
    const double c = 0.55991073;
    if (e <= 0.5) return (e * e) / 3.0;
    return (std::exp((e - c) / a) + b) / 12.0;
}

// ITU-R BT.709 OETF.
static double bt709Oetf(double L) {
    if (L < 0.0) L = 0.0;
    if (L > 1.0) L = 1.0;
    if (L < 0.018) return 4.5 * L;
    return 1.099 * std::pow(L, 0.45) - 0.099;
}

// Banana custom tone map. Replaces Hable (which even at white=4.0
// left SDR-range pixels at 50-85 % output brightness - noticeably
// dark on typical HDR films). Design goals:
//   1. SDR-referred values (linear light in [0, 0.9]) pass through
//      1:1 - shadows and midtones keep their native brightness.
//   2. HDR highlights (x > 0.9) soft-knee compress toward the
//      display peak (1.0) with a hyperbolic asymptote, so specular
//      highlights (sun, explosions, direct lights) don't hard-clip
//      but also don't visibly compress midtones.
// Result vs Hable(4.0):
//   x=0.5 (50 nit)     : Banana 0.50 vs Hable 0.21
//   x=1.0 (SDR white)  : Banana 0.93 vs Hable 0.50
//   x=2.0 (200 nit)    : Banana 0.97 vs Hable 0.89
//   x=10  (HDR peak)   : Banana 0.998 vs Hable 1.00
// Bump kHdrLutCacheVersion whenever kBananaKnee or kBananaShoulderK change.
static constexpr double kBananaKnee       = 0.90;
static constexpr double kBananaShoulderK  = 0.20;   // smaller = tighter shoulder
static double bananaToneMap(double x) {
    if (x <= 0.0) return 0.0;
    if (x <= kBananaKnee) return x;
    double excess = x - kBananaKnee;
    double soft   = excess / (excess + kBananaShoulderK);   // in [0, 1)
    return kBananaKnee + (1.0 - kBananaKnee) * soft;
}
// (Hable filmic reference: y = ((x*(0.15*x+0.05)+0.004)/(x*(0.15*x+0.5)+0.06)) - 0.067;
//  kept as documentation only - we compared against it during tuning
//  and Banana's knee curve is brighter for typical HDR content.)

// BT.2020 -> BT.709 gamut conversion matrix, applied to linear-light RGB.
static const double kBt2020ToBt709[3][3] = {
    { 1.6605, -0.5876, -0.0728},
    {-0.1246,  1.1329, -0.0083},
    {-0.0182, -0.1006,  1.1187},
};

// BT.2020 non-constant-luminance YUV -> R'G'B' (all still gamma-encoded).
// Coefficients derived from Kr=0.2627, Kb=0.0593 (BT.2020 luma weights).
static inline void bt2020NclYuvToRgb(double Y, double U, double V,
                                     double& R, double& G, double& B) {
    R = Y + 1.4746   * V;
    G = Y - 0.16455  * U - 0.57135 * V;
    B = Y + 1.8814   * U;
}

// BT.2020 luminance weights (for the HLG OOTF).
static const double kBt2020LumaR = 0.2627;
static const double kBt2020LumaG = 0.6780;
static const double kBt2020LumaB = 0.0593;

VideoPlayer::VideoPlayer() {}
VideoPlayer::~VideoPlayer() { unload(); }

// ---------- hardware decode setup ----------

// Static callback used by FFmpeg to let us pick a hardware pixel format from
// the list it offers for this frame. Returning the HW format tells FFmpeg to
// emit hardware-backed AVFrames; returning a SW format quietly disables HW.
AVPixelFormat VideoPlayer::pickHwPixFormat(AVCodecContext* ctx,
                                           const AVPixelFormat* pix_fmts) {
    auto* self = static_cast<VideoPlayer*>(ctx->opaque);
    if (!self) return pix_fmts[0];
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->hw_pix_fmt_) return *p;
    }
    // HW format not offered - fall back to whatever FFmpeg wants (SW decode).
    self->hw_active_ = false;
    self->hw_name_   = "software";
    return pix_fmts[0];
}

bool VideoPlayer::tryInitHwDecode(AVCodecContext* codec_ctx, const AVCodec* codec) {
    // Escape hatch for testing / triage: BP_NO_HW=1 disables hardware
    // decode entirely so the SW path is exercised. Useful when a driver
    // silently accepts a codec/profile combo but produces zero frames.
    if (const char* v = std::getenv("BP_NO_HW"); v && v[0] == '1') return false;
    // Preference order: D3D11VA (modern, minimal quirks) then DXVA2. Anything
    // else on Windows tends to be vendor-specific and we want a universal
    // default, so those two cover most GPUs.
    const struct { AVHWDeviceType type; const char* label; } candidates[] = {
        { AV_HWDEVICE_TYPE_D3D11VA, "D3D11VA" },
        { AV_HWDEVICE_TYPE_DXVA2,   "DXVA2"   },
    };
    for (const auto& cand : candidates) {
        // Verify this codec advertises HW support for this device type.
        AVPixelFormat hw_fmt = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                && cfg->device_type == cand.type) {
                hw_fmt = cfg->pix_fmt;
                break;
            }
        }
        if (hw_fmt == AV_PIX_FMT_NONE) continue;

        AVBufferRef* dev = nullptr;
        if (av_hwdevice_ctx_create(&dev, cand.type, nullptr, nullptr, 0) < 0) {
            continue;
        }

        hw_device_ctx_    = dev;
        hw_pix_fmt_       = hw_fmt;
        hw_name_          = cand.label;
        codec_ctx->opaque       = this;
        codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        codec_ctx->get_format    = pickHwPixFormat;
        return true;
    }
    return false;
}

void VideoPlayer::unload() {
    quit_ = true;
    playing_ = false;

    { std::lock_guard<std::mutex> lk(video_queue_mutex_); video_queue_cv_.notify_all(); }
    { std::lock_guard<std::mutex> lk(video_pkt_mutex_);   video_pkt_cv_.notify_all(); }
    { std::lock_guard<std::mutex> lk(audio_pkt_mutex_);   audio_pkt_cv_.notify_all(); }

    // Join in the order that avoids deadlock: demux first (stops
    // pushing packets), then the two decoders (drain their queues).
    if (decode_thread_.joinable()) decode_thread_.join();
    if (video_decode_thread_.joinable()) video_decode_thread_.join();
    if (audio_decode_thread_.joinable()) audio_decode_thread_.join();
    loaded_ = false;

    // Drain leftover packets from either queue - the joined threads
    // may have left some behind if they woke up on quit_.
    {
        std::lock_guard<std::mutex> lk(video_pkt_mutex_);
        while (!video_pkt_queue_.empty()) {
            AVPacket* p = video_pkt_queue_.front();
            av_packet_free(&p);
            video_pkt_queue_.pop_front();
        }
    }
    {
        std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
        while (!audio_pkt_queue_.empty()) {
            AVPacket* p = audio_pkt_queue_.front();
            av_packet_free(&p);
            audio_pkt_queue_.pop_front();
        }
    }

    if (audio_stream_) {
        SDL_DestroyAudioStream(audio_stream_);
        audio_stream_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(video_queue_mutex_);
        while (!video_queue_.empty()) {
            AVFrame* f = video_queue_.front().frame;
            av_frame_free(&f);
            video_queue_.pop_front();
        }
    }
    {
        std::lock_guard<std::mutex> lk(last_frame_mutex_);
        if (last_frame_) av_frame_free(&last_frame_);
    }
    clearHistory_();
    deferred_display_pts_.store(-1.0, std::memory_order_release);

    if (swr_ctx_) swr_free(&swr_ctx_);
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (sws_hdr_from_rgb_) { sws_freeContext(sws_hdr_from_rgb_); sws_hdr_from_rgb_ = nullptr; }
    hdr_kind_ = HdrKind::None;
    hdr_input_lut_.clear();
    hdr_out_lut_.clear();
    hdr_hlg_gain_lut_.clear();
    freeDeinterlaceGraph_();
    source_interlaced_ = false;
    closeEmbeddedSubtitleDecoder_();
    embedded_sub_tracks_.clear();
    active_embedded_sub_.store(-1);
    if (video_dec_ctx_) avcodec_free_context(&video_dec_ctx_);
    if (audio_dec_ctx_) avcodec_free_context(&audio_dec_ctx_);
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);

    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
    video_width_ = 0;
    video_height_ = 0;
    video_sar_ = {1, 1};
    video_color_range_ = AVCOL_RANGE_UNSPECIFIED;
    video_color_space_ = AVCOL_SPC_UNSPECIFIED;
    duration_ = 0.0;
    filename_.clear();
    quit_ = false;
    seek_requested_ = false;
    eof_ = false;
    ended_signaled_ = false;
    audio_pushed_bytes_ = 0;
    audio_seek_base_time_ = 0.0;
    base_time_ = 0.0;
    // MediaClock: reset to (0, paused). loadFile will re-anchor.
    media_anchor_pts_  = 0.0;
    media_anchor_perf_ = 0;
    media_paused_      = true;
    media_seek_pending_ = false;
    media_seek_pending_since_perf_ = 0;
    audio_base_needs_capture_ = false;
    audio_skip_to_pts_.store(-1e18, std::memory_order_relaxed);
    video_skip_to_pts_.store(-1e18, std::memory_order_relaxed);
    playback_rate_ = 1.0f;
    av_sync_offset_ = 0.0;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    hw_active_  = false;
    hw_name_    = "software";
}

bool VideoPlayer::loadFile(const std::string& filename, int preferred_audio_stream) {
    unload();
    filename_ = filename;
    bpProbe("loadFile begin path=\"%s\"", filename.c_str());

    // Wall-clock deadline for the open+probe path so a stalled network
    // share, a spun-down drive, or a slow URL doesn't freeze the main
    // thread. Deadline is stored in the AVFormatContext via `interrupt_callback`
    // and reset every time we start a fresh open.
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) return false;
    open_deadline_ = SDL_GetTicks() + 5000;   // 5 seconds
    fmt_ctx_->interrupt_callback.callback = &VideoPlayer::openInterruptCb;
    fmt_ctx_->interrupt_callback.opaque   = this;

    // I/O buffering options. Big-file / external-HDD / network sources
    // choke with the FFmpeg default (32 KB AVIO buffer, ~4 ms of
    // headroom at 60 Mbps 4K HDR bitrate) - every read hits the disk
    // and any Windows-side stall (indexing sweep, drive cache flush,
    // ...) starves the decoder. 8 MB gives ~1 s of buffer for typical
    // 4K HDR, enough to ride out most spikes. probesize + analyzeduration
    // are also bumped so the initial stream probe on a fresh open
    // reads enough to identify tricky HDR/DV containers.
    AVDictionary* io_opts = nullptr;
    av_dict_set(&io_opts, "buffer_size",       "8388608",  0);   // 8 MB
    av_dict_set(&io_opts, "probesize",         "16777216", 0);   // 16 MB
    av_dict_set(&io_opts, "analyzeduration",   "3000000",  0);   // 3 s

    if (avformat_open_input(&fmt_ctx_, filename.c_str(), nullptr, &io_opts) < 0) {
        av_dict_free(&io_opts);
        bpProbe("loadFile fail stage=avformat_open_input");
        unload();
        return false;
    }
    av_dict_free(&io_opts);
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        bpProbe("loadFile fail stage=find_stream_info fmt=%s",
                fmt_ctx_->iformat ? fmt_ctx_->iformat->name : "?");
        unload();
        return false;
    }
    // Open+probe done - disarm the deadline so it doesn't fire during
    // steady-state reads (seeks, EOF, mid-file HW-fallback re-probes).
    open_deadline_ = 0;

    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVCodecParameters* par = fmt_ctx_->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx_ < 0) {
            video_stream_idx_ = (int)i;
        }
    }

    // Choose audio stream: either user-preferred, or first available.
    if (preferred_audio_stream >= 0
        && preferred_audio_stream < (int)fmt_ctx_->nb_streams
        && fmt_ctx_->streams[preferred_audio_stream]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        audio_stream_idx_ = preferred_audio_stream;
    } else {
        for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
            if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_idx_ = (int)i;
                break;
            }
        }
    }

    // Audio-only containers (.m4a / .mp3 / .flac / voice memos) are
    // supported: skip video setup when there's no video stream but audio
    // is present. Callers use isAudioOnly() to decide whether to draw a
    // placeholder in the video area. Fail only when neither stream exists.
    if (video_stream_idx_ < 0 && audio_stream_idx_ < 0) {
        bpProbe("loadFile fail stage=no_playable_stream fmt=%s nb_streams=%u",
                fmt_ctx_->iformat ? fmt_ctx_->iformat->name : "?",
                fmt_ctx_->nb_streams);
        unload();
        return false;
    }

    // Video decoder (skipped for audio-only files)
    if (video_stream_idx_ >= 0) {
        AVCodecParameters* par = fmt_ctx_->streams[video_stream_idx_]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            bpProbe("loadFile fail stage=no_video_decoder codec_id=%d", (int)par->codec_id);
            unload(); return false;
        }
        video_dec_ctx_ = avcodec_alloc_context3(codec);
        if (!video_dec_ctx_) { bpProbe("loadFile fail stage=alloc_video_ctx"); unload(); return false; }
        if (avcodec_parameters_to_context(video_dec_ctx_, par) < 0) {
            bpProbe("loadFile fail stage=params_to_ctx"); unload(); return false;
        }
        video_dec_ctx_->thread_count = 0;

        // Attempt HW decode BEFORE avcodec_open2 - the codec picks its path
        // when opened. If HW init fails at any level we silently keep SW.
        hw_active_ = tryInitHwDecode(video_dec_ctx_, codec);

        if (avcodec_open2(video_dec_ctx_, codec, nullptr) < 0) {
            // HW-attached open failed. Free the attachment and retry SW.
            if (video_dec_ctx_->hw_device_ctx) av_buffer_unref(&video_dec_ctx_->hw_device_ctx);
            video_dec_ctx_->get_format = nullptr;
            video_dec_ctx_->opaque     = nullptr;
            // Also drop our own reference to the HW device context -
            // otherwise the orphan GPU device handle stays alive for
            // the entire file's lifetime and only gets released on
            // unload / file switch. Leaks D3D11 device on every file
            // where HW init succeeded partially then had to fall back.
            if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
            hw_pix_fmt_ = AV_PIX_FMT_NONE;
            hw_active_ = false;
            hw_name_   = "software";
            if (avcodec_open2(video_dec_ctx_, codec, nullptr) < 0) {
                bpProbe("loadFile fail stage=avcodec_open2 codec=%s", codec->name);
                unload(); return false;
            }
        }
        video_width_ = video_dec_ctx_->width;
        video_height_ = video_dec_ctx_->height;
        video_time_base_ = fmt_ctx_->streams[video_stream_idx_]->time_base;
        // Capture SAR up front so the render path can display anamorphic
        // sources (e.g. 1920x1080 tagged 607:1920) at the intended aspect
        // instead of squishing them to 16:9. codecpar SAR beats the decoder
        // ctx one because the container-side value is set even before the
        // first frame decodes.
        {
            AVRational s = fmt_ctx_->streams[video_stream_idx_]
                                ->codecpar->sample_aspect_ratio;
            if (s.num <= 0 || s.den <= 0) s = {1, 1};
            video_sar_ = s;
        }

        // Capture color range + matrix from codecpar so the renderer can pin
        // the correct SDL_Colorspace on the YUV texture. The default Full
        // BT.709 that SDL would otherwise map Limited-range content against
        // lifts blacks across virtually all distribution content.
        {
            AVCodecParameters* cp =
                fmt_ctx_->streams[video_stream_idx_]->codecpar;
            video_color_range_ = cp->color_range;
            video_color_space_ = cp->color_space;
        }

        // HDR classification is informational only until we have a real
        // tone-map pipeline (see HdrKind doc in the header).
        AVCodecParameters* vpar = fmt_ctx_->streams[video_stream_idx_]->codecpar;
        switch (vpar->color_trc) {
            case AVCOL_TRC_SMPTE2084:    hdr_kind_ = HdrKind::PQ;  break;
            case AVCOL_TRC_ARIB_STD_B67: hdr_kind_ = HdrKind::HLG; break;
            default:                     hdr_kind_ = HdrKind::None; break;
        }
        // Decide the effective tone-map output size for this file.
        // Default Off: turned out that most "4K HDR stutters" traced
        // to disk-I/O starvation (AVIO buffer, not CPU tone-map). Auto
        // is still available for users on genuinely slow CPUs; Off is
        // the default so nobody loses visual fidelity by accident.
        // ForceOn overrides even for sub-4K sources.
        HdrHalfResMode m = hdr_half_res_mode_.load();
        bool want_half = (m == HdrHalfResMode::ForceOn)
                      || (m == HdrHalfResMode::Auto && video_width_ >= 3840);
        hdr_half_res_active_ = want_half && hdr_kind_ != HdrKind::None;
        // High-res SDR downscale. Only for the SDR path - HDR uses its
        // own half-res axis above. Auto engages when either:
        //   1) source > 3840 wide (5K/6K/8K); or
        //   2) pixel-rate (W*H*fps) > 300 Mpx/s, which catches 4K/60 but
        //      leaves 4K/30 and 1440p60 alone. Rationale: at 4K/60 the
        //      HW-decode -> CPU download -> sws -> upload round-trip is
        //      ~750 MB/s of memory bandwidth, and on some systems that
        //      pressure starves the SDL audio device thread just enough
        //      to cause repeated 'tick-tick' underruns. Halving output
        //      dims quarters the bandwidth and typically eliminates it,
        //      at a cost most viewers can't see on a 1080p/1440p display.
        //   ForceOn overrides for any size, Off disables. See setter doc
        //   in the header for rationale.
        HighResDownscaleMode hd = hires_downscale_mode_.load();
        double src_fps = 0.0;
        {
            AVRational fr = fmt_ctx_->streams[video_stream_idx_]->r_frame_rate;
            if (fr.num == 0 || fr.den == 0) {
                fr = fmt_ctx_->streams[video_stream_idx_]->avg_frame_rate;
            }
            if (fr.num > 0 && fr.den > 0) src_fps = av_q2d(fr);
        }
        double pix_rate = (double)video_width_ * (double)video_height_ * src_fps;
        bool want_hires_down = (hd == HighResDownscaleMode::ForceOn)
                            || (hd == HighResDownscaleMode::Auto
                                && (video_width_ > 3840 || pix_rate > 300e6));
        hires_downscale_active_ = want_hires_down && hdr_kind_ == HdrKind::None;
        // Interlaced source detection. Uses the codec's field_order tag
        // (AV_FIELD_TT/BB/TB/BT). Progressive and unknown are treated as
        // non-interlaced so we don't gratuitously smear progressive
        // material. Auto mode acts on this flag.
        AVFieldOrder fo = vpar->field_order;
        source_interlaced_ = (fo == AV_FIELD_TT || fo == AV_FIELD_BB
                           || fo == AV_FIELD_TB || fo == AV_FIELD_BT);
    }

    // Audio decoder (optional)
    if (audio_stream_idx_ >= 0) {
        AVCodecParameters* par = fmt_ctx_->streams[audio_stream_idx_]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        bool ok = false;
        if (codec) {
            audio_dec_ctx_ = avcodec_alloc_context3(codec);
            if (audio_dec_ctx_
                && avcodec_parameters_to_context(audio_dec_ctx_, par) == 0
                && avcodec_open2(audio_dec_ctx_, codec, nullptr) == 0) {
                audio_time_base_ = fmt_ctx_->streams[audio_stream_idx_]->time_base;
                audio_sample_rate_ = audio_dec_ctx_->sample_rate > 0 ? audio_dec_ctx_->sample_rate : 44100;
                audio_channels_ = 2;
                audio_frame_bytes_ = audio_channels_ * 2;

                SDL_AudioSpec spec;
                spec.freq = audio_sample_rate_;
                spec.format = SDL_AUDIO_S16;
                spec.channels = audio_channels_;
                audio_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                         &spec, nullptr, nullptr);
                if (audio_stream_) {
                    AVChannelLayout in_layout = {};
                    if (audio_dec_ctx_->ch_layout.nb_channels > 0) {
                        av_channel_layout_copy(&in_layout, &audio_dec_ctx_->ch_layout);
                    } else {
                        av_channel_layout_default(&in_layout, 2);
                    }
                    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                    if (swr_alloc_set_opts2(&swr_ctx_,
                            &out_layout, AV_SAMPLE_FMT_S16, audio_sample_rate_,
                            &in_layout, audio_dec_ctx_->sample_fmt, audio_dec_ctx_->sample_rate,
                            0, nullptr) == 0 && swr_init(swr_ctx_) >= 0) {
                        applyGain();
                        SDL_PauseAudioStreamDevice(audio_stream_);
                        ok = true;
                    }
                    av_channel_layout_uninit(&in_layout);
                }
            }
        }
        if (!ok) {
            if (swr_ctx_) swr_free(&swr_ctx_);
            if (audio_stream_) { SDL_DestroyAudioStream(audio_stream_); audio_stream_ = nullptr; }
            if (audio_dec_ctx_) avcodec_free_context(&audio_dec_ctx_);
            audio_stream_idx_ = -1;
        }
    }

    // Audio-only files with a broken audio decoder (e.g. HE-AAC v2 in an
    // FFmpeg build without SBR+21 support) would otherwise "succeed" with
    // nothing playable at all. Fail cleanly so UIController surfaces the
    // usual "Cannot open" toast instead of a silent zombie load.
    if (video_stream_idx_ < 0 && audio_stream_idx_ < 0) {
        bpProbe("loadFile fail stage=audio_decoder_open fmt=%s",
                fmt_ctx_->iformat ? fmt_ctx_->iformat->name : "?");
        unload();
        return false;
    }

    // Note: sws_ctx_ is now created lazily during decode via
    // sws_getCachedContext because HW decode paths produce different pixel
    // formats than video_dec_ctx_->pix_fmt (typically NV12 after transfer).

    duration_ = (fmt_ctx_->duration != AV_NOPTS_VALUE)
                    ? (fmt_ctx_->duration / (double)AV_TIME_BASE)
                    : 0.0;

    // Enumerate embedded subtitle streams. Text formats are marked
    // selectable; bitmap formats (PGS, DVB, DVD sub) are included in
    // the list but marked non-selectable so the UI can present them
    // greyed out. `language` / `title` are pulled from stream metadata
    // when the container set them.
    embedded_sub_tracks_.clear();
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream* st = fmt_ctx_->streams[i];
        if (!st || st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        EmbeddedSubtitleTrack t;
        t.stream_idx = (int)i;
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        t.codec_name = c ? c->name : "unknown";
        t.selectable = (st->codecpar->codec_id == AV_CODEC_ID_SUBRIP
                     || st->codecpar->codec_id == AV_CODEC_ID_ASS
                     || st->codecpar->codec_id == AV_CODEC_ID_SSA
                     || st->codecpar->codec_id == AV_CODEC_ID_TEXT
                     || st->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT
                     // Bitmap formats - renderable via the ABGR32
                     // upload path in UIController::drawSubtitles.
                     || st->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE
                     || st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE
                     || st->codecpar->codec_id == AV_CODEC_ID_DVD_SUBTITLE
                     || st->codecpar->codec_id == AV_CODEC_ID_XSUB);
        if (auto* tag = av_dict_get(st->metadata, "language", nullptr, 0)) {
            if (tag->value) t.language = tag->value;
        }
        if (auto* tag = av_dict_get(st->metadata, "title", nullptr, 0)) {
            if (tag->value) t.title = tag->value;
        }
        embedded_sub_tracks_.push_back(std::move(t));
    }

    // Reset auto-normalization for the new file. Mode itself persists
    // across files (it's a user preference); only the analysis state
    // and the derived gain reset.
    auto_gain_.store(1.0f);
    peak_bytes_analyzed_.store(0);
    peak_max_abs_.store(0.0f);

    loaded_ = true;
    quit_ = false;
    eof_ = false;
    ended_signaled_ = false;
    first_video_frame_probed_ = false;
    video_frames_pushed_.store(0);
    video_frames_displayed_.store(0);
    video_packet_send_errors_.store(0);
    demux_video_packets_.store(0);
    video_recv_frame_errors_.store(0);
    video_packets_sent_.store(0);
    decoder_stuck_.store(false);
    audio_pushed_bytes_ = 0;
    audio_seek_base_time_ = 0.0;
    base_time_ = 0.0;
    start_perf_ = SDL_GetPerformanceCounter();
    // MediaClock: anchor at 0, frozen. play() unfreezes; first audio
    // nudge hard-anchors to actual start pts.
    mediaAnchorLocked_(0.0);
    media_paused_ = true;
    media_seek_pending_ = (audio_stream_idx_ >= 0);
    media_seek_pending_since_perf_ = media_seek_pending_
        ? SDL_GetPerformanceCounter() : 0;
    audio_base_needs_capture_ = (audio_stream_idx_ >= 0);
    first_play_needed_ = true;

    if (video_stream_idx_ >= 0) {
        video_decode_thread_ = std::thread(&VideoPlayer::videoDecodeThread, this);
    }
    if (audio_stream_idx_ >= 0 && audio_dec_ctx_ && audio_stream_) {
        audio_decode_thread_ = std::thread(&VideoPlayer::audioDecodeThread, this);
    }
    decode_thread_ = std::thread(&VideoPlayer::demuxDecodeThread, this);
    bpProbe("loadFile ok fmt=%s vcodec=%s vwh=%dx%d pix=%d trc=%d "
            "acodec=%s asr=%d ach=%d hw=%s dur=%.3f nb_streams=%u",
            fmt_ctx_->iformat ? fmt_ctx_->iformat->name : "?",
            video_dec_ctx_ && video_dec_ctx_->codec ? video_dec_ctx_->codec->name : "?",
            video_width_, video_height_,
            video_dec_ctx_ ? (int)video_dec_ctx_->pix_fmt : -1,
            video_dec_ctx_ ? (int)video_dec_ctx_->color_trc : -1,
            audio_dec_ctx_ && audio_dec_ctx_->codec ? audio_dec_ctx_->codec->name : "-",
            audio_dec_ctx_ ? audio_dec_ctx_->sample_rate : 0,
            audio_dec_ctx_ ? audio_dec_ctx_->ch_layout.nb_channels : 0,
            hw_name_ ? hw_name_ : "?",
            fmt_ctx_ && fmt_ctx_->duration != AV_NOPTS_VALUE
                ? (double)fmt_ctx_->duration / (double)AV_TIME_BASE : -1.0,
            fmt_ctx_ ? fmt_ctx_->nb_streams : 0);
    return true;
}

void VideoPlayer::play() {
    if (!loaded_) return;
    if (eof_) seek(0.0);
    if (playing_) return;
    // If the user is currently viewing an old history frame (walked back
    // with `,`), resume playback from THAT frame's pts - not from where
    // the master clock was frozen before the walk. Seek there and clear
    // the ring so normal playback continues cleanly. clearHistory_() is
    // called by seek() itself.
    {
        int off = history_offset_.load(std::memory_order_acquire);
        if (off > 0) {
            double resume_pts = 0.0;
            bool   have = false;
            {
                std::lock_guard<std::mutex> lk(history_mutex_);
                int idx = (int)frame_history_.size() - 1 - off;
                if (idx >= 0) { resume_pts = frame_history_[(size_t)idx].pts; have = true; }
            }
            if (have) {
                seek(resume_pts);   // implicitly clears history + resets offset
            }
        }
    }
    // "First play after load" must NOT be detected by inspecting
    // audio_pushed_bytes_ - decode thread may not have pushed a single
    // byte by the time loadPath calls play() (fast CPU, slow first-packet
    // read), and then we'd skip the un-stick dance and freeze on frame 0.
    // Use the explicit flag set at the end of loadFile.
    bool need_dance = first_play_needed_.exchange(false) && audio_stream_;
    {
        std::lock_guard<std::mutex> lk(clock_mutex_);
        // Legacy field kept fresh for any residual reader; MediaClock
        // is the actual source of truth.
        start_perf_ = SDL_GetPerformanceCounter();
        mediaSetPausedLocked_(false);
    }
    ended_signaled_ = false;
    playing_ = true;
    just_unfroze_.store(true);
    if (audio_stream_) {
        // Two symptoms observed on Explorer double-click of an mp4:
        //   (a) video stuck at frame 0, fixed by manual Space+Space.
        //   (b) video stuck at frame 0, fixed by a seek.
        //
        // Both trace to the same failure mode - `SDL_GetAudioStreamQueued`
        // never decreases after loadFile+play(), so the audio-master
        // clock stays pinned at audio_seek_base_time_ (0) and updateTexture
        // can only release the pts~0 frame. The single ResumeAudioStreamDevice
        // that used to live here got silently coalesced with the Pause
        // done during loadFile's audio init on some Windows configs.
        //
        // What (a) does: real Pause -> real Resume state transitions via
        // the UI's togglePause path, with a full frame between them.
        // What (b) does: seek's flushQueues() calls SDL_ClearAudioStream
        // and resetClockLocked() zeroes audio_pushed_bytes_, giving SDL
        // a clean state to restart from.
        //
        // Fix here does BOTH for the first play after load: clear the
        // stream, zero the byte counter, then Pause -> Delay -> Resume
        // so SDL's device thread can't coalesce the transition. The
        // ~50 ms of audio that was queued during loadFile is discarded;
        // decode continues from the next demuxer packet, which is a
        // small A/V desync at start that self-corrects as the audio
        // master clock catches up. For subsequent play() calls (after
        // a user-initiated pause) we just do the idempotent Pause+Resume
        // since the device thread is already in a healthy state.
        if (need_dance) {
            SDL_ClearAudioStream(audio_stream_);
            std::lock_guard<std::mutex> lk(clock_mutex_);
            audio_pushed_bytes_ = 0;
            // Any pts capture that happened while decode was queuing
            // pre-play bytes is now stale - we just threw those bytes
            // out. Re-arm capture so the next audio frame after the
            // clear re-seeds the seek base with the true first-heard
            // pts. Without this the heard-position estimate is biased
            // by the pts of frames that never actually reached SDL.
            audio_base_needs_capture_ = true;
            media_seek_pending_       = true;
            media_seek_pending_since_perf_ = SDL_GetPerformanceCounter();
        }
        SDL_PauseAudioStreamDevice(audio_stream_);
        if (need_dance) SDL_Delay(20);   // let SDL's device thread ack the pause
        SDL_ResumeAudioStreamDevice(audio_stream_);
    }
}

void VideoPlayer::pause() {
    if (!loaded_) return;
    if (!playing_) return;
    {
        std::lock_guard<std::mutex> lk(clock_mutex_);
        if (audio_stream_idx_ < 0) {
            base_time_ = getMasterClockLocked();
        }
        // Freeze MediaClock at current position - nudges from any late
        // audio pushes will no-op while paused, so this becomes the
        // authoritative resume point.
        mediaSetPausedLocked_(true);
    }
    playing_ = false;
    if (audio_stream_) SDL_PauseAudioStreamDevice(audio_stream_);
}

void VideoPlayer::togglePause() {
    if (playing_) pause(); else play();
}

void VideoPlayer::seekInternal_(double seconds, bool preserve_deferred) {
    if (!loaded_) return;
    double d = duration_ > 0.0 ? duration_ : 0.0;
    seconds = std::clamp(seconds, 0.0, d);
    // Any seek invalidates the frame history - the frames it holds are
    // no longer contiguous with the new playback position.
    clearHistory_();
    // Only a step-back seek should preserve its deferred-display target.
    // Regular seeks land at whatever keyframe FFmpeg gives us (via
    // AVSEEK_FLAG_BACKWARD) - that keyframe frame is displayed as-is,
    // no "decode past to exact target" attempt. Precise seeks are a
    // deliberate non-goal: they either delay video behind audio (bad
    // AV split) or thrash allocations pushing pre-target frames aside.
    // Every mainstream player lands at the keyframe for exactly this
    // reason. Audio was also re-seeked to the same fmt_ctx position,
    // so first audio pts is near the keyframe pts - the first audio
    // nudge hard-anchors MediaClock there and playback proceeds
    // normally from the keyframe.
    if (!preserve_deferred) {
        // When paused: arm deferred-display at the seek target so
        // updateTexture silently drains all pre-target frames (from
        // the previous keyframe up to target) and shows ONLY the
        // target frame. Matches other players' "seek while paused
        // = snap to that frame instantly" behavior instead of the
        // brief GOP-replay-fast-play the user sees otherwise.
        // When playing: clear deferred; normal release path takes
        // over so pre-target frames drop as stale and audio-nudged
        // MediaClock releases target-and-after frames in real time.
        if (!playing_.load()) {
            deferred_display_pts_.store(seconds, std::memory_order_release);
        } else {
            deferred_display_pts_.store(-1.0, std::memory_order_release);
        }
    }
    seek_target_ = seconds;
    seek_requested_ = true;
    eof_ = false;
    ended_signaled_ = false;
    { std::lock_guard<std::mutex> lk(video_queue_mutex_); video_queue_cv_.notify_all(); }
}

void VideoPlayer::seekRelative(double delta) {
    if (!loaded_) return;
    seek(getCurrentTime() + delta);
}

void VideoPlayer::stepFrame(double delta) {
    if (!loaded_) return;
    if (delta < 0.0) {
        bool was_playing = playing_;
        if (was_playing) pause();
        // Walk the frame history if we have a prior frame in it.
        // updateTexture picks up the new offset next tick and re-uploads
        // the target frame from the ring - no seek, no GOP replay.
        double seek_base = 0.0;
        bool   have_base = false;
        {
            std::lock_guard<std::mutex> lk(history_mutex_);
            int off = history_offset_.load(std::memory_order_relaxed);
            int size = (int)frame_history_.size();
            if (off + 1 < size) {
                history_offset_.store(off + 1, std::memory_order_release);
                return;
            }
            // Ring exhausted. Seek target must be measured from the frame
            // we're CURRENTLY VIEWING (the oldest in the ring, index 0),
            // NOT from the master clock. The clock froze at whatever pts
            // was live when the user paused, which is `size` frames after
            // the oldest ring entry - seeking from there would repeatedly
            // land on a scene the user has already walked past, and the
            // subsequent GOP decode would look like the scene "repeats".
            if (size > 0) {
                seek_base = frame_history_[0].pts;
                have_base = true;
            }
        }
        double base = have_base ? seek_base : getCurrentTime();
        // Desired target: exactly one frame back from where we're viewing.
        // Approximate frame duration via getVideoFrameRate; fall back to
        // 33.3 ms if unknown.
        double fps    = getVideoFrameRate();
        double one_dt = (fps > 0.0) ? 1.0 / fps : 1.0 / 30.0;
        double target = std::max(0.0, base - one_dt);
        // Seek to ~500 ms EARLIER so the decoder produces a batch of
        // frames spanning the intended target. deferred_display_pts_
        // tells updateTexture to silently drain the queue into the
        // ring until it sees the target frame, so the user never sees
        // the pre-target GOP replay.
        double seek_to = std::max(0.0, target - 0.500);
        deferred_display_pts_.store(target, std::memory_order_release);
        seekInternal_(seek_to, /*preserve_deferred=*/true);
        return;
    }
    // Forward step. If we're currently walked back inside the history
    // ring, take a step toward newest instead of advancing the clock -
    // that keeps us inside the ring and eventually lands back at
    // offset 0 (normal playback state).
    {
        std::lock_guard<std::mutex> lk(history_mutex_);
        int off = history_offset_.load(std::memory_order_relaxed);
        if (off > 0) {
            history_offset_.store(off - 1, std::memory_order_release);
            return;
        }
    }
    // At the newest history entry (offset 0). Advance the clock so the
    // next queued frame gets released. Cap at duration - 50 ms and fall
    // back to a real seek if the queue is empty or the proposed clock
    // would race past the latest decoded frame - either case would
    // otherwise freeze the picture until the decoder catches up.
    double cap = (duration_ > 0.0) ? duration_ - 0.050 : 1e18;
    double proposed;
    {
        std::lock_guard<std::mutex> lk(clock_mutex_);
        double base = (audio_stream_idx_ >= 0 && audio_stream_)
                    ? audio_seek_base_time_
                    : base_time_;
        proposed = std::min(base + delta, cap);
    }
    bool need_seek = false;
    {
        std::lock_guard<std::mutex> lk(video_queue_mutex_);
        if (video_queue_.empty()) {
            need_seek = true;
        } else {
            double tail = video_queue_.back().pts;
            if (proposed > tail + 1.5) need_seek = true;
        }
    }
    if (need_seek) {
        seek(proposed);
        return;
    }
    std::lock_guard<std::mutex> lk(clock_mutex_);
    if (audio_stream_idx_ >= 0 && audio_stream_) {
        audio_seek_base_time_ = proposed;
    } else {
        base_time_ = proposed;
    }
}

void VideoPlayer::pushHistory_(AVFrame* frame, double pts) {
    if (!frame) return;
    AVFrame* clone = av_frame_clone(frame);
    if (!clone) return;
    pushHistoryTakingOwnership_(clone, pts);
}

void VideoPlayer::pushHistoryTakingOwnership_(AVFrame* frame, double pts) {
    if (!frame) return;
    std::lock_guard<std::mutex> lk(history_mutex_);
    // Rough byte cost: for YUV420P at width*height plane sizes,
    // total = w*h + 2*(w/2)*(h/2) = w*h*1.5. Works for other planar
    // 4:2:0 formats too; for anything else this is a slight underestimate
    // but the budget is generous.
    uint64_t bytes = (uint64_t)std::max(1, frame->width)
                   * (uint64_t)std::max(1, frame->height) * 3 / 2;
    frame_history_.push_back({frame, pts});
    history_bytes_ += bytes;
    while (history_bytes_ > kHistoryBytesCap && frame_history_.size() > 1) {
        AVFrame* front = frame_history_.front().frame;
        int fw = std::max(1, front->width);
        int fh = std::max(1, front->height);
        uint64_t fb = (uint64_t)fw * (uint64_t)fh * 3 / 2;
        av_frame_free(&front);
        frame_history_.pop_front();
        if (history_bytes_ > fb) history_bytes_ -= fb; else history_bytes_ = 0;
    }
}

void VideoPlayer::clearHistory_() {
    std::lock_guard<std::mutex> lk(history_mutex_);
    while (!frame_history_.empty()) {
        AVFrame* f = frame_history_.front().frame;
        av_frame_free(&f);
        frame_history_.pop_front();
    }
    history_bytes_ = 0;
    history_offset_.store(0, std::memory_order_release);
}

double VideoPlayer::getCurrentTime() const {
    std::lock_guard<std::mutex> lk(clock_mutex_);
    return getMasterClockLocked();
}

int VideoPlayer::openInterruptCb(void* opaque) {
    auto* self = static_cast<VideoPlayer*>(opaque);
    if (!self) return 0;
    if (self->quit_.load() || self->seek_requested_.load()) return 1;
    const auto deadline = self->open_deadline_.load();
    return deadline && SDL_GetTicks() >= deadline;
}

const char* VideoPlayer::getVideoCodecName() const {
    if (!video_dec_ctx_ || !video_dec_ctx_->codec) return "";
    const char* n = avcodec_get_name(video_dec_ctx_->codec_id);
    return n ? n : "";
}

const char* VideoPlayer::getAudioCodecName() const {
    if (!audio_dec_ctx_ || !audio_dec_ctx_->codec) return "";
    const char* n = avcodec_get_name(audio_dec_ctx_->codec_id);
    return n ? n : "";
}

double VideoPlayer::getVideoFrameRate() const {
    if (!fmt_ctx_ || video_stream_idx_ < 0) return 0.0;
    AVStream* st = fmt_ctx_->streams[video_stream_idx_];
    // Prefer `r_frame_rate` (the "real" base frame rate that all timestamps
    // are integer multiples of); fall back to `avg_frame_rate` when the
    // real rate is unset (some containers only fill one of them).
    AVRational r = st->r_frame_rate;
    if (r.num == 0 || r.den == 0) r = st->avg_frame_rate;
    if (r.num == 0 || r.den == 0) return 0.0;
    return (double)r.num / (double)r.den;
}

int64_t VideoPlayer::getFileBitrate() const {
    if (!fmt_ctx_) return 0;
    return fmt_ctx_->bit_rate;
}

// Pull a metadata tag from an AVStream (returns "" when absent).
static std::string streamTag(AVStream* st, const char* key) {
    if (!st || !st->metadata) return std::string();
    AVDictionaryEntry* e = av_dict_get(st->metadata, key, nullptr, 0);
    return (e && e->value) ? std::string(e->value) : std::string();
}

// Convert an FFmpeg channel layout to a human-readable name like
// "stereo" or "5.1(side)". Returns "" when the layout is invalid.
static std::string channelLayoutName(const AVChannelLayout& layout) {
    char buf[128] = {0};
    int n = av_channel_layout_describe(&layout, buf, sizeof(buf));
    if (n <= 0) return std::string();
    return std::string(buf);
}

VideoPlayer::MediaDetail VideoPlayer::getMediaDetail() const {
    MediaDetail d;
    if (!fmt_ctx_) return d;

    // ---- Container ----
    if (fmt_ctx_->iformat) {
        d.container_short = fmt_ctx_->iformat->name ? fmt_ctx_->iformat->name : "";
        d.container_long  = fmt_ctx_->iformat->long_name ? fmt_ctx_->iformat->long_name : "";
    }
    d.num_streams    = fmt_ctx_->nb_streams;
    d.num_programs   = fmt_ctx_->nb_programs;
    d.format_bitrate = fmt_ctx_->bit_rate;
    d.probe_score    = fmt_ctx_->probe_score;
    d.start_time_us  = fmt_ctx_->start_time;

    // ---- Video (active stream) ----
    if (video_stream_idx_ >= 0
        && video_stream_idx_ < (int)fmt_ctx_->nb_streams) {
        AVStream* vst = fmt_ctx_->streams[video_stream_idx_];
        AVCodecParameters* p = vst->codecpar;
        d.has_video = true;

        d.video_codec_short = avcodec_get_name(p->codec_id) ? avcodec_get_name(p->codec_id) : "";
        {
            const AVCodecDescriptor* cd = avcodec_descriptor_get(p->codec_id);
            if (cd && cd->long_name) d.video_codec_long = cd->long_name;
        }
        // Profile name via avcodec_profile_name (needs the codec, not just id).
        {
            const AVCodec* c = avcodec_find_decoder(p->codec_id);
            if (c) {
                const char* pn = av_get_profile_name(c, p->profile);
                if (pn) d.video_profile = pn;
            }
            if (d.video_profile.empty() && p->profile > 0) {
                d.video_profile = std::to_string(p->profile);
            }
        }
        d.video_level  = p->level;
        d.video_width  = p->width;
        d.video_height = p->height;
        d.video_bits_per_component = p->bits_per_raw_sample > 0
                                   ? p->bits_per_raw_sample
                                   : p->bits_per_coded_sample;

        AVPixelFormat pf = (AVPixelFormat)p->format;
        if (pf != AV_PIX_FMT_NONE) {
            const char* n = av_get_pix_fmt_name(pf);
            if (n) d.video_pix_fmt = n;
        }

        auto safeName = [](const char* n) { return n ? std::string(n) : std::string(); };
        d.video_color_space     = safeName(av_color_space_name(p->color_space));
        d.video_color_range     = safeName(av_color_range_name(p->color_range));
        d.video_color_primaries = safeName(av_color_primaries_name(p->color_primaries));
        d.video_color_transfer  = safeName(av_color_transfer_name(p->color_trc));
        d.video_chroma_location = safeName(av_chroma_location_name(p->chroma_location));

        switch (p->field_order) {
            case AV_FIELD_PROGRESSIVE: d.video_field_order = "Progressive"; break;
            case AV_FIELD_TT: d.video_field_order = "Interlaced TFF"; break;
            case AV_FIELD_BB: d.video_field_order = "Interlaced BFF"; break;
            case AV_FIELD_TB: d.video_field_order = "Interlaced TB";  break;
            case AV_FIELD_BT: d.video_field_order = "Interlaced BT";  break;
            default:          d.video_field_order = "Unknown";        break;
        }

        // SAR from codecpar (may be 0/1). DAR = SAR * (W/H).
        d.video_sar = p->sample_aspect_ratio;
        if (d.video_sar.num == 0 || d.video_sar.den == 0) {
            d.video_sar.num = 1; d.video_sar.den = 1;
        }
        if (p->width > 0 && p->height > 0) {
            AVRational src_ratio{ p->width, p->height };
            AVRational dar = av_mul_q(d.video_sar, src_ratio);
            av_reduce(&d.video_dar.num, &d.video_dar.den, dar.num, dar.den, 1024 * 1024);
        }

        AVRational rfr = vst->r_frame_rate;
        AVRational afr = vst->avg_frame_rate;
        d.video_fps     = (rfr.num > 0 && rfr.den > 0) ? av_q2d(rfr) : 0.0;
        d.video_avg_fps = (afr.num > 0 && afr.den > 0) ? av_q2d(afr) : 0.0;
        d.video_bitrate = p->bit_rate;
        d.video_nb_frames = vst->nb_frames;
        if (hw_active_) d.hw_decoder = hw_name_ ? hw_name_ : "";

        d.video_language = streamTag(vst, "language");
        d.video_title    = streamTag(vst, "title");

        // Keyframe interval: walk the container's index and average the
        // distance between adjacent AVINDEX_KEYFRAME entries. Works for
        // MP4/MOV/MKV/FLV (which all populate the index during
        // avformat_find_stream_info). Falls back to 0.0 when the format
        // demuxer doesn't build an index (raw H.264 elementary streams,
        // live HLS, ...). Sample the first ~200 keyframes so a 3h movie
        // doesn't cost a full-index walk. Time base is video_time_base_
        // populated in loadFile.
        int nb = avformat_index_get_entries_count(vst);
        if (nb > 1) {
            const int cap = 200;
            int keyframes = 0;
            int64_t first_ts = AV_NOPTS_VALUE, last_ts = AV_NOPTS_VALUE;
            for (int i = 0; i < nb && keyframes < cap; ++i) {
                const AVIndexEntry* e = avformat_index_get_entry(vst, i);
                if (!e) continue;
                if (e->flags & AVINDEX_KEYFRAME) {
                    if (first_ts == AV_NOPTS_VALUE) first_ts = e->timestamp;
                    last_ts = e->timestamp;
                    keyframes++;
                }
            }
            d.keyframe_count = keyframes;
            if (keyframes > 1 && last_ts != AV_NOPTS_VALUE
                && first_ts != AV_NOPTS_VALUE) {
                double span = (last_ts - first_ts) * av_q2d(vst->time_base);
                d.keyframe_interval_sec = span / (double)(keyframes - 1);
            }
        }
    }

    // ---- Audio (active track) ----
    if (audio_stream_idx_ >= 0
        && audio_stream_idx_ < (int)fmt_ctx_->nb_streams) {
        AVStream* ast = fmt_ctx_->streams[audio_stream_idx_];
        AVCodecParameters* p = ast->codecpar;
        d.has_audio = true;

        d.audio_codec_short = avcodec_get_name(p->codec_id) ? avcodec_get_name(p->codec_id) : "";
        {
            const AVCodecDescriptor* cd = avcodec_descriptor_get(p->codec_id);
            if (cd && cd->long_name) d.audio_codec_long = cd->long_name;
        }
        {
            const AVCodec* c = avcodec_find_decoder(p->codec_id);
            if (c) {
                const char* pn = av_get_profile_name(c, p->profile);
                if (pn) d.audio_profile = pn;
            }
            if (d.audio_profile.empty() && p->profile > 0) {
                d.audio_profile = std::to_string(p->profile);
            }
        }
        d.audio_sample_rate = p->sample_rate;
        d.audio_channels    = p->ch_layout.nb_channels;
        d.audio_channel_layout = channelLayoutName(p->ch_layout);
        AVSampleFormat sf = (AVSampleFormat)p->format;
        if (sf != AV_SAMPLE_FMT_NONE) {
            const char* n = av_get_sample_fmt_name(sf);
            if (n) d.audio_sample_fmt = n;
        }
        d.audio_bits_per_sample = p->bits_per_raw_sample > 0
                                ? p->bits_per_raw_sample
                                : p->bits_per_coded_sample;
        d.audio_bitrate  = p->bit_rate;
        d.audio_language = streamTag(ast, "language");
        d.audio_title    = streamTag(ast, "title");
    }

    // ---- Subtitle streams ----
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream* st = fmt_ctx_->streams[i];
        if (!st || !st->codecpar) continue;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
        MediaDetail::SubStreamInfo info;
        info.codec_short = avcodec_get_name(st->codecpar->codec_id)
                        ? avcodec_get_name(st->codecpar->codec_id) : "";
        info.language = streamTag(st, "language");
        info.title    = streamTag(st, "title");
        AVCodecID cid = st->codecpar->codec_id;
        info.selectable = (cid == AV_CODEC_ID_SUBRIP
                        || cid == AV_CODEC_ID_ASS
                        || cid == AV_CODEC_ID_SSA
                        || cid == AV_CODEC_ID_TEXT
                        || cid == AV_CODEC_ID_MOV_TEXT);
        d.subtitle_streams.push_back(std::move(info));
    }

    // ---- Attachments ----
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream* st = fmt_ctx_->streams[i];
        if (st && st->codecpar
            && st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
            d.num_attachments++;
        }
    }

    return d;
}

int VideoPlayer::getIntrinsicRotation() const {
    if (!fmt_ctx_ || video_stream_idx_ < 0) return 0;
    AVStream* st = fmt_ctx_->streams[video_stream_idx_];
    if (!st || !st->codecpar) return 0;

    auto snapToBucket = [](double theta_cw) -> int {
        // Normalize to [0, 360) then snap to nearest 90-degree bucket.
        int cw = (int)std::lround(theta_cw);
        cw = ((cw % 360) + 360) % 360;
        if (cw >=  45 && cw < 135) return 90;
        if (cw >= 135 && cw < 225) return 180;
        if (cw >= 225 && cw < 315) return 270;
        return 0;
    };

    // Preferred: display matrix side data on codecpar (FFmpeg 5+).
    // av_display_rotation_get returns the counter-clockwise angle needed
    // to rotate the frame into the CORRECT display orientation, so we
    // negate to get the clockwise angle SDL_RenderTextureRotated wants.
    for (int i = 0; i < st->codecpar->nb_coded_side_data; ++i) {
        const AVPacketSideData* sd = &st->codecpar->coded_side_data[i];
        if (sd->type == AV_PKT_DATA_DISPLAYMATRIX
            && sd->size >= 9 * sizeof(int32_t)) {
            double theta_ccw = av_display_rotation_get(
                                    (const int32_t*)sd->data);
            if (!std::isnan(theta_ccw)) return snapToBucket(-theta_ccw);
        }
    }
    // Fallback: some containers (older mp4, mov) still expose a bare
    // `rotate=N` string in stream metadata even though FFmpeg's own
    // deprecation removed the automatic bridge to display matrix on
    // some code paths. Cheap to check.
    AVDictionaryEntry* tag = av_dict_get(st->metadata, "rotate", nullptr, 0);
    if (tag && tag->value) {
        return snapToBucket(std::atof(tag->value));
    }
    return 0;
}

bool VideoPlayer::isStillImage() const {
    if (!video_dec_ctx_) return false;
    switch (video_dec_ctx_->codec_id) {
        case AV_CODEC_ID_PNG:
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_JPEG2000:
        case AV_CODEC_ID_JPEGLS:
        case AV_CODEC_ID_BMP:
        case AV_CODEC_ID_WEBP:
        case AV_CODEC_ID_TIFF:
        case AV_CODEC_ID_TARGA:
        case AV_CODEC_ID_PCX:
        case AV_CODEC_ID_PPM:
        case AV_CODEC_ID_PGM:
        case AV_CODEC_ID_PBM:
        case AV_CODEC_ID_QOI:
            return true;
        // Note: AV_CODEC_ID_GIF intentionally NOT here - animated GIFs are
        // a legitimate multi-frame video-like case, and single-frame GIFs
        // just play once and stay on the last frame like a static image.
        default:
            return false;
    }
}

void VideoPlayer::resetClockLocked(double t) {
    // Legacy fields kept updated so any residual read paths (info panel,
    // etc.) still see coherent values. The MediaClock freezes at the
    // seek target until the first audio frame after the flush arrives,
    // at which point it hard-anchors to the true audio position.
    start_perf_ = SDL_GetPerformanceCounter();
    if (audio_stream_idx_ >= 0) {
        audio_pushed_bytes_ = 0;
        audio_seek_base_time_ = t;
        // Capture actual first-frame pts so per-file container offsets
        // (AAC padding, start_time, MKV audio offsets) don't bake in
        // a constant lip-sync error.
        audio_base_needs_capture_ = true;
        // Freeze MediaClock until first nudge lands (prevents post-
        // seek fast-forward artifact).
        media_seek_pending_ = true;
        media_seek_pending_since_perf_ = SDL_GetPerformanceCounter();
    } else {
        base_time_ = t;
    }
    mediaAnchorLocked_(t);
}

// Default assumption for audio output latency between SDL "consuming"
// a byte from the queued stream and that byte actually reaching the
// user's ear. See the setter docs in the header for the tuning story;
// runtime value lives in `audio_out_latency_ms_`.
constexpr int kAudioOutputLatencyDefaultMs = 30;

// MediaClock: wall-clock-anchored media time source. Video release logic
// reads it via getCurrentTime() and sees a smooth number that grows with
// SDL_GetPerformanceCounter (no SDL audio-chunk jitter). When audio is
// present the decode thread calls mediaNudgeLocked_() every push with a
// "what the user is hearing right now" estimate; small deltas nudge the
// anchor 5 % of the way toward audio, large deltas hard-snap. Long-term
// the clock stays audio-locked; short-term video pacing stays smooth
// even when audio delivery bunches into 20-40 ms chunks.
double VideoPlayer::mediaNowLocked_() const {
    // Freeze in three cases:
    //   - paused: user-facing pause
    //   - seek_pending: waiting for first audio frame to hard-anchor;
    //     ticking here would let video race ahead during the decode-
    //     recovery gap, causing the visible fast-forward-after-seek.
    //     Safety valve: if audio hasn't arrived within
    //     kMediaSeekPendingTimeoutMs (~500 ms) the clock ticks anyway,
    //     otherwise files that emit a long run of video before the
    //     first audio packet (mp4s with negative-DTS edit list -
    //     e.g. Twitter clips) livelock with video pinned to the first
    //     frame forever, waiting for an audio nudge that can't happen
    //     until the demuxer moves past all those video packets, which
    //     it can't because the video queue is full and the decode
    //     thread is blocked.
    //   - anchor_perf==0: uninitialized (right after loadFile)
    if (media_paused_ || media_anchor_perf_ == 0) {
        return media_anchor_pts_;
    }
    uint64_t now = SDL_GetPerformanceCounter();
    if (media_seek_pending_) {
        uint64_t since = media_seek_pending_since_perf_;
        if (since == 0) return media_anchor_pts_;
        double waited_ms = (double)(now - since) * 1000.0
                        / (double)SDL_GetPerformanceFrequency();
        constexpr double kMediaSeekPendingTimeoutMs = 500.0;
        if (waited_ms < kMediaSeekPendingTimeoutMs) return media_anchor_pts_;
        // Timed out - promote the freeze into a normal ticking clock.
        // Advance the anchor to "now" and clear the pending flag so a
        // late-arriving first audio nudge does a soft 5 % correction
        // instead of hard-anchoring the clock backward (which would
        // freeze video for the wall-clock time we ticked past).
        double elapsed_since_anchor = (double)(now - media_anchor_perf_)
                                    / (double)SDL_GetPerformanceFrequency();
        double rate = (double)playback_rate_.load();
        media_anchor_pts_  += elapsed_since_anchor * rate;
        media_anchor_perf_  = now;
        media_seek_pending_ = false;
        media_seek_pending_since_perf_ = 0;
        return media_anchor_pts_;
    }
    double elapsed = (double)(now - media_anchor_perf_)
                   / (double)SDL_GetPerformanceFrequency();
    double rate = (double)playback_rate_.load();
    return media_anchor_pts_ + elapsed * rate;
}

void VideoPlayer::mediaAnchorLocked_(double pts) {
    media_anchor_pts_  = pts;
    media_anchor_perf_ = SDL_GetPerformanceCounter();
}

void VideoPlayer::mediaSetPausedLocked_(bool paused) {
    if (paused) {
        // Freeze at current media time.
        media_anchor_pts_  = mediaNowLocked_();
        media_anchor_perf_ = SDL_GetPerformanceCounter();
        media_paused_      = true;
    } else {
        // Resume from last anchored position; refresh perf so
        // elapsed since resume starts at zero.
        media_anchor_perf_ = SDL_GetPerformanceCounter();
        media_paused_      = false;
    }
}

void VideoPlayer::mediaNudgeLocked_(double heard_pts) {
    if (media_paused_) return;
    // First nudge after seek/loadFile: hard-anchor to actual audio
    // position and unfreeze. This clamps out any drift accumulated
    // during the decode-recovery gap so video doesn't fast-forward
    // to catch a stale MediaClock.
    if (media_seek_pending_) {
        media_anchor_pts_    = heard_pts;
        media_anchor_perf_   = SDL_GetPerformanceCounter();
        media_seek_pending_  = false;
        media_seek_pending_since_perf_ = 0;
        return;
    }
    double my_now = mediaNowLocked_();
    double delta  = heard_pts - my_now;
    double absd   = delta < 0 ? -delta : delta;
    // Dead zone: don't churn for sub-frame jitter (< 5 ms).
    if (absd < 0.005) return;
    // Large deviation - hard snap. Covers pathological cases where
    // steady-state falls out of sync (audio device restart, driver
    // hiccup, etc). Threshold lowered from 200 to 50 ms since the
    // main "big delta" case (post-seek gap) is now handled by the
    // media_seek_pending_ branch above.
    if (absd > 0.050) {
        media_anchor_pts_  = heard_pts;
        media_anchor_perf_ = SDL_GetPerformanceCounter();
        return;
    }
    // Gentle drift correction: 5 % pull per nudge. With audio pushes
    // arriving several times per second, we converge in under a second
    // without visible video pacing jitter.
    media_anchor_pts_  = my_now + delta * 0.05;
    media_anchor_perf_ = SDL_GetPerformanceCounter();
}

double VideoPlayer::getMasterClockLocked() const {
    // MediaClock is the single source of truth. Video release path
    // (via getCurrentTime()) reads this and gets a smooth, wall-clock
    // interpolated number. When audio is present, mediaNudgeLocked_()
    // keeps the anchor pulled toward "what the user is hearing right
    // now" so the value converges to audio position over time. When
    // audio is absent (image-only files, muted-decode) the MediaClock
    // is anchored on play() and just ticks with wall time.
    (void)start_perf_; (void)base_time_; (void)audio_pushed_bytes_;
    (void)audio_seek_base_time_;   // legacy fields kept for callers that snapshot state
    return mediaNowLocked_();
}

void VideoPlayer::setVolume(float v) {
    // 0.0 = silent, 1.0 = source level, up to 2.0 for boosted playback on
    // quiet sources. SDL_SetAudioStreamGain happily accepts values > 1.
    volume_ = std::clamp(v, 0.0f, 2.0f);
    applyGain();
}

void VideoPlayer::setMuted(bool m) {
    muted_ = m;
    applyGain();
}

void VideoPlayer::setHighResDownscaleMode(HighResDownscaleMode m) {
    hires_downscale_mode_.store(m);
    // Re-evaluate the active flag against the current source so a
    // Settings toggle applies to the file already playing. HDR still
    // has its own hdr_half_res axis; hires only affects the SDR path
    // (materializeYUV's sws_scale output). Next frame the display
    // texture in main.cpp will notice getOutputWidth changed and
    // rebuild at the new size; sws_getCachedContext handles the
    // input->output size change transparently.
    if (loaded_.load() && hdr_kind_ == HdrKind::None && video_width_ > 0) {
        double src_fps = 0.0;
        if (fmt_ctx_ && video_stream_idx_ >= 0) {
            AVRational fr = fmt_ctx_->streams[video_stream_idx_]->r_frame_rate;
            if (fr.num == 0 || fr.den == 0) {
                fr = fmt_ctx_->streams[video_stream_idx_]->avg_frame_rate;
            }
            if (fr.num > 0 && fr.den > 0) src_fps = av_q2d(fr);
        }
        double pix_rate = (double)video_width_ * (double)video_height_ * src_fps;
        bool want = (m == HighResDownscaleMode::ForceOn)
                 || (m == HighResDownscaleMode::Auto
                     && (video_width_ > 3840 || pix_rate > 300e6));
        hires_downscale_active_ = want;
    }
}

int VideoPlayer::getOutputWidth() const {
    if (hdr_half_res_active_) {
        int w = video_width_ / 2;
        return (w > 0) ? (w & ~1) : video_width_;   // round to even
    }
    if (hires_downscale_active_) {
        int w = video_width_ / 2;
        return (w > 0) ? (w & ~1) : video_width_;
    }
    return video_width_;
}

int VideoPlayer::getOutputHeight() const {
    if (hdr_half_res_active_) {
        int h = video_height_ / 2;
        return (h > 0) ? (h & ~1) : video_height_;
    }
    if (hires_downscale_active_) {
        int h = video_height_ / 2;
        return (h > 0) ? (h & ~1) : video_height_;
    }
    return video_height_;
}

// ---------- Deinterlacing filter graph (yadif) ----------

void VideoPlayer::freeDeinterlaceGraph_() {
    if (filter_graph_) {
        avfilter_graph_free(&filter_graph_);
    }
    filter_graph_  = nullptr;
    buffersrc_     = nullptr;
    buffersink_    = nullptr;
    filter_in_fmt_ = AV_PIX_FMT_NONE;
    filter_in_w_ = 0;
    filter_in_h_ = 0;
    filter_in_tb_ = AVRational{0, 1};
}

bool VideoPlayer::buildDeinterlaceGraph_(AVPixelFormat pix_fmt, int W, int H,
                                         AVRational time_base) {
    freeDeinterlaceGraph_();
    filter_graph_ = avfilter_graph_alloc();
    if (!filter_graph_) return false;

    const AVFilter* src_flt = avfilter_get_by_name("buffer");
    const AVFilter* snk_flt = avfilter_get_by_name("buffersink");
    if (!src_flt || !snk_flt) { freeDeinterlaceGraph_(); return false; }

    char args[256];
    std::snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1",
        W, H, (int)pix_fmt, time_base.num, time_base.den);
    if (avfilter_graph_create_filter(&buffersrc_, src_flt, "in", args,
                                     nullptr, filter_graph_) < 0) {
        freeDeinterlaceGraph_(); return false;
    }
    if (avfilter_graph_create_filter(&buffersink_, snk_flt, "out", nullptr,
                                     nullptr, filter_graph_) < 0) {
        freeDeinterlaceGraph_(); return false;
    }

    // Insert yadif in "send_frame" mode (mode=0): one output per input,
    // no framerate doubling. `deint=all` runs on every frame regardless
    // of the frame-level interlaced flag so an On/Auto decision at load
    // time isn't second-guessed per frame.
    const AVFilter* yadif = avfilter_get_by_name("yadif");
    if (!yadif) { freeDeinterlaceGraph_(); return false; }
    AVFilterContext* yadif_ctx = nullptr;
    if (avfilter_graph_create_filter(&yadif_ctx, yadif, "yadif",
                                     "mode=0:parity=auto:deint=all",
                                     nullptr, filter_graph_) < 0) {
        freeDeinterlaceGraph_(); return false;
    }
    if (avfilter_link(buffersrc_, 0, yadif_ctx, 0) < 0
        || avfilter_link(yadif_ctx, 0, buffersink_, 0) < 0) {
        freeDeinterlaceGraph_(); return false;
    }
    if (avfilter_graph_config(filter_graph_, nullptr) < 0) {
        freeDeinterlaceGraph_(); return false;
    }
    filter_in_fmt_ = pix_fmt;
    filter_in_w_   = W;
    filter_in_h_   = H;
    filter_in_tb_  = time_base;
    return true;
}

void VideoPlayer::setDeinterlaceMode(DeinterlaceMode m) {
    if (deinterlace_mode_.exchange(m) != m) {
        deinterlace_rebuild_.store(true);
    }
}

void VideoPlayer::applyGain() {
    if (audio_stream_) {
        float g = muted_ ? 0.0f
                         : volume_.load() * auto_gain_.load();
        SDL_SetAudioStreamGain(audio_stream_, g);
    }
}

void VideoPlayer::setNormalizeMode(NormalizeMode m) {
    NormalizeMode prev = normalize_mode_.exchange(m);
    if (prev == m) return;
    // Reset analysis so the current file re-samples. If disabling, snap
    // gain back to 1.0 immediately so the user hears the change.
    peak_bytes_analyzed_.store(0);
    peak_max_abs_.store(0.0f);
    if (m == NormalizeMode::Off) {
        auto_gain_.store(1.0f);
        applyGain();
    }
}

void VideoPlayer::samplePeak_(const int16_t* samples, int byte_count) {
    if (normalize_mode_.load() == NormalizeMode::Off) return;
    uint64_t already = peak_bytes_analyzed_.load();
    uint64_t target  = (uint64_t)audio_sample_rate_ * audio_frame_bytes_
                       * kPeakAnalysisTargetSec;
    if (already >= target) return;
    const int n_samples = byte_count / (int)sizeof(int16_t);
    const float inv = 1.0f / 32767.0f;
    float local_max = peak_max_abs_.load();
    for (int i = 0; i < n_samples; ++i) {
        int16_t s = samples[i];
        float a = ((s < 0) ? (float)(-(int)s) : (float)s) * inv;
        if (a > local_max) local_max = a;
    }
    peak_max_abs_.store(local_max);
    peak_bytes_analyzed_.store(already + (uint64_t)byte_count);
    if (already + (uint64_t)byte_count >= target) {
        finalizeAutoGain_();
    }
}

void VideoPlayer::finalizeAutoGain_() {
    float peak = peak_max_abs_.load();
    if (peak <= 0.0001f) {
        // Silent or near-silent - leave gain at 1.0.
        auto_gain_.store(1.0f);
        applyGain();
        return;
    }
    NormalizeMode m = normalize_mode_.load();
    float target = (m == NormalizeMode::Strong) ? 0.98f
                 : (m == NormalizeMode::Soft)   ? 0.90f
                                                : 1.0f;
    float g = target / peak;
    // Clamp: never quiet a source, cap at 4x boost so a whispered clip
    // doesn't blow the tweeters. Users who need more can still push
    // volume past 100% manually.
    if (g < 1.0f) g = 1.0f;
    if (g > 4.0f) g = 4.0f;
    auto_gain_.store(g);
    applyGain();
}

void VideoPlayer::setAVSyncOffset(double s) {
    if (s < -1.0) s = -1.0;
    if (s >  1.0) s =  1.0;
    av_sync_offset_.store(s);
}

void VideoPlayer::setAudioOutputLatencyMs(int ms) {
    if (ms < 0)   ms = 0;
    if (ms > 500) ms = 500;
    audio_out_latency_ms_.store(ms);
}

void VideoPlayer::setPlaybackRate(float r) {
    r = std::clamp(r, 0.25f, 3.0f);
    std::lock_guard<std::mutex> lk(clock_mutex_);
    if (audio_stream_idx_ < 0) {
        base_time_ = getMasterClockLocked();
        start_perf_ = SDL_GetPerformanceCounter();
    }
    // Re-anchor MediaClock at current media time BEFORE the rate change
    // so mediaNowLocked_() returns the same value continuously across
    // the transition (the new rate applies to future elapsed only).
    mediaAnchorLocked_(mediaNowLocked_());
    playback_rate_.store(r);
    if (audio_stream_) {
        SDL_SetAudioStreamFrequencyRatio(audio_stream_, r);
    }
}

// Upper bound of the linear-light values fed into out_lut. After the
// BT.2020->BT.709 gamut expansion the biggest positive value we can hit
// is ~1.66 * peak_input; for a 1000-nit PQ source (peak_input == 10 on
// our SDR-normalized scale) that's ~16.6. 20.0 gives generous headroom
// and Hable is heavily saturated up there anyway.
static constexpr float kHdrLutMaxLinear    = 20.0f;
static constexpr float kHdrLutInvMaxLinear = 65535.0f / kHdrLutMaxLinear;

// banana.hdrlut cache file layout. All four tables are content-invariant
// (they depend only on the constants/functions above, not on the video),
// so once computed they can be reused forever. Bump kHdrLutCacheVersion
// whenever ANY of these change: pqEotf/hlgInvOetf/hableMap/bt709Oetf
// formulas, kHdrLutMaxLinear, the HLG OOTF alpha/gamma, the peak-nits
// normalization, or the entry count. Otherwise stale caches will apply
// the old curves to new algorithm assumptions.
static constexpr uint32_t kHdrLutCacheMagic   = 0x5455444Cu;  // 'LDUT'
// v3: Hable -> bananaToneMap. SDR range now passes through 1:1;
// only HDR highlights are compressed. Much brighter midtones on
// dark-mastered HDR films.
static constexpr uint32_t kHdrLutCacheVersion = 3;

static std::string hdrLutCachePath() {
    // Prefer user-writable path (%APPDATA%\BananaPlayer\) since exe-adjacent
    // (SDL_GetBasePath) is typically read-only under Program Files.
    // SDL_GetPrefPath creates the directory as a side effect. Empty
    // org gives us "%APPDATA%\BananaPlayer\", matching UIController::dataDir.
    const char* pref = SDL_GetPrefPath("", "BananaPlayer");
    if (pref && *pref) return std::string(pref) + "banana.hdrlut";
    const char* base = SDL_GetBasePath();
    if (!base) return std::string("banana.hdrlut");
    return std::string(base) + "banana.hdrlut";
}

// Compute all four LUTs from scratch. Extracted so the loader can call
// it after a cache miss without duplicating the math.
static void computeHdrLuts(std::vector<float>& pq_in,
                           std::vector<float>& hlg_in,
                           std::vector<uint16_t>& out,
                           std::vector<float>& hlg_gain) {
    pq_in.resize(65536);
    hlg_in.resize(65536);
    out.resize(65536);
    hlg_gain.resize(65536);
    for (int i = 0; i < 65536; ++i) {
        double e = (double)i / 65535.0;
        pq_in[i]    = (float)(pqEotf(e) / 100.0);
        hlg_in[i]   = (float)hlgInvOetf(e);
        hlg_gain[i] = (float)(10.0 * std::pow(e, 0.2));

        double linear  = (double)i / kHdrLutInvMaxLinear;
        double mapped  = bananaToneMap(linear);
        if (mapped < 0.0) mapped = 0.0;
        if (mapped > 1.0) mapped = 1.0;
        double encoded = bt709Oetf(mapped);
        int v = (int)std::lround(encoded * 65535.0);
        out[i] = (uint16_t)std::clamp(v, 0, 65535);
    }
}

// Read the cache file into the four vectors. Any failure (missing file,
// wrong magic/version, short read) returns false and leaves the outputs
// untouched. The pq_in / hlg_in / hlg_gain vectors are always populated
// together (all four tables are cheap to hold and future files may switch
// HDR kind mid-session).
static bool loadHdrLutCache(std::vector<float>& pq_in,
                            std::vector<float>& hlg_in,
                            std::vector<uint16_t>& out,
                            std::vector<float>& hlg_gain) {
    std::string path = hdrLutCachePath();
    std::FILE* f = FileIO::open(path.c_str(), "rb");
    if (!f) return false;
    uint32_t hdr[2] = {0, 0};
    if (std::fread(hdr, sizeof(uint32_t), 2, f) != 2
        || hdr[0] != kHdrLutCacheMagic
        || hdr[1] != kHdrLutCacheVersion) {
        std::fclose(f);
        return false;
    }
    std::vector<float>    tmp_pq(65536);
    std::vector<float>    tmp_hlg(65536);
    std::vector<uint16_t> tmp_out(65536);
    std::vector<float>    tmp_gain(65536);
    bool ok = std::fread(tmp_pq.data(),   sizeof(float),    65536, f) == 65536
           && std::fread(tmp_hlg.data(),  sizeof(float),    65536, f) == 65536
           && std::fread(tmp_out.data(),  sizeof(uint16_t), 65536, f) == 65536
           && std::fread(tmp_gain.data(), sizeof(float),    65536, f) == 65536;
    std::fclose(f);
    if (!ok) return false;
    pq_in    = std::move(tmp_pq);
    hlg_in   = std::move(tmp_hlg);
    out      = std::move(tmp_out);
    hlg_gain = std::move(tmp_gain);
    return true;
}

// Persist the four tables to disk. Best-effort: any I/O failure is
// swallowed silently - the LUTs still work this session, we just re-pay
// the compute cost next launch.
static void saveHdrLutCache(const std::vector<float>& pq_in,
                            const std::vector<float>& hlg_in,
                            const std::vector<uint16_t>& out,
                            const std::vector<float>& hlg_gain) {
    if (pq_in.size() != 65536 || hlg_in.size() != 65536
        || out.size() != 65536 || hlg_gain.size() != 65536) return;
    std::string path = hdrLutCachePath();
    std::FILE* f = FileIO::open(path.c_str(), "wb");
    if (!f) return;
    uint32_t hdr[2] = { kHdrLutCacheMagic, kHdrLutCacheVersion };
    std::fwrite(hdr, sizeof(uint32_t), 2, f);
    std::fwrite(pq_in.data(),    sizeof(float),    65536, f);
    std::fwrite(hlg_in.data(),   sizeof(float),    65536, f);
    std::fwrite(out.data(),      sizeof(uint16_t), 65536, f);
    std::fwrite(hlg_gain.data(), sizeof(float),    65536, f);
    std::fclose(f);
}

void VideoPlayer::ensureHdrLuts_() {
    // Populated already this session? Fast return.
    const bool have_out  = (hdr_out_lut_.size() == 65536);
    const bool have_gain = (hdr_hlg_gain_lut_.size() == 65536);
    const bool have_in   = (hdr_input_lut_.size() == 65536);
    if (have_out && have_gain && have_in) return;

    // The four tables are content-invariant, so we always load/populate
    // them all together - the extra 640 KB of RAM is trivial and means
    // switching HDR kind mid-session doesn't re-hit the cache path.
    std::vector<float>    pq_in;
    std::vector<float>    hlg_in;
    std::vector<uint16_t> out;
    std::vector<float>    hlg_gain;
    if (!loadHdrLutCache(pq_in, hlg_in, out, hlg_gain)) {
        computeHdrLuts(pq_in, hlg_in, out, hlg_gain);
        saveHdrLutCache(pq_in, hlg_in, out, hlg_gain);
    }
    // Point hdr_input_lut_ at whichever variant matches the current file.
    // Both variants stay resident in the cache-load stack copies but only
    // one is retained; a future file switch that flips PQ<->HLG will
    // re-enter this function and rebuild via the cache path (fast).
    hdr_out_lut_      = std::move(out);
    hdr_hlg_gain_lut_ = std::move(hlg_gain);
    hdr_input_lut_    = (hdr_kind_ == HdrKind::PQ) ? std::move(pq_in)
                                                    : std::move(hlg_in);
}

AVFrame* VideoPlayer::materializeHdrToneMap_(AVFrame* src_in) {
    // HW frames get transferred to SW first - the transferred format is
    // typically P010LE (D3D11VA / DXVA2 on Windows). SW-decoded HDR HEVC
    // Main10 shows up as YUV420P10LE. Both are handled below.
    AVFrame* tmp_sw = nullptr;
    AVFrame* src = src_in;
    if (hw_active_ && src_in->format == hw_pix_fmt_) {
        tmp_sw = av_frame_alloc();
        if (!tmp_sw || av_hwframe_transfer_data(tmp_sw, src_in, 0) < 0) {
            if (tmp_sw) av_frame_free(&tmp_sw);
            return nullptr;
        }
        src = tmp_sw;
    }

    const AVPixelFormat srcFmt = (AVPixelFormat)src->format;
    // Recognise the 10-bit planar formats we can walk by hand. Anything
    // else falls back to the SDR path (washed-out picture is better than
    // wrong-color picture).
    int bit_depth = 0;
    bool is_p010 = false;   // planar Y + interleaved UV
    switch (srcFmt) {
        case AV_PIX_FMT_YUV420P10LE: bit_depth = 10; is_p010 = false; break;
        case AV_PIX_FMT_YUV420P12LE: bit_depth = 12; is_p010 = false; break;
        case AV_PIX_FMT_P010LE:      bit_depth = 10; is_p010 = true;  break;
        default:
            if (tmp_sw) av_frame_free(&tmp_sw);
            return nullptr;
    }

    ensureHdrLuts_();

    // Source dims (used to walk the input YUV planes) vs output dims
    // (what we hand SDL). Half-res mode drops the output size; the
    // pixel loop still walks the source at full stride but writes into
    // a smaller RGB48 buffer via nearest-neighbor sampling.
    const int W = src->width;
    const int H = src->height;
    const int outW = hdr_half_res_active_ ? (W / 2) & ~1 : W;
    const int outH = hdr_half_res_active_ ? (H / 2) & ~1 : H;
    const int step_x = hdr_half_res_active_ ? 2 : 1;
    const int step_y = hdr_half_res_active_ ? 2 : 1;

    // Alloc the intermediate SDR RGB48LE buffer at OUTPUT size.
    AVFrame* rgb = av_frame_alloc();
    if (!rgb) { if (tmp_sw) av_frame_free(&tmp_sw); return nullptr; }
    rgb->format = AV_PIX_FMT_RGB48LE;
    rgb->width  = outW;
    rgb->height = outH;
    if (av_frame_get_buffer(rgb, 32) != 0) {
        av_frame_free(&rgb);
        if (tmp_sw) av_frame_free(&tmp_sw);
        return nullptr;
    }

    // Range constants for limited-range N-bit YUV:
    //   Y  black = 16<<(N-8), white = 235<<(N-8)
    //   UV neutral = 128<<(N-8), full swing = (240-16)<<(N-8) (peak to peak)
    // Values live in the low N bits (yuv420p10le) or the top 10 bits of a
    // 16-bit word (p010le - hence the >>6 shift below).
    const int   shift        = is_p010 ? (16 - bit_depth) : 0;
    const float y_black      = (float)(16  << (bit_depth - 8));
    const float inv_y_scale  = 1.0f / (float)((235 - 16) << (bit_depth - 8));
    const float uv_neutral   = (float)(128 << (bit_depth - 8));
    const float inv_uv_scale = 1.0f / (float)((240 - 16) << (bit_depth - 8));

    // ensureHdrLuts_() above guarantees all three tables are sized to
    // 65536. Grabbing raw pointers here is safe - if any of the LUTs
    // were somehow empty (they can't be) the pixel loop would UB, so
    // no defensive nullptr wrap: the invariant lives with the setup.
    const float*    in_lut      = hdr_input_lut_.data();
    const uint16_t* out_lut     = hdr_out_lut_.data();
    const float*    hlg_gainlut = hdr_hlg_gain_lut_.data();
    const bool      is_hlg      = (hdr_kind_ == HdrKind::HLG);

    // BT.2020 -> BT.709 gamut matrix as floats (double copies are slow to
    // reload each iteration; local floats stay in registers).
    const float m00 = 1.6605f, m01 = -0.5876f, m02 = -0.0728f;
    const float m10 = -0.1246f, m11 = 1.1329f, m12 = -0.0083f;
    const float m20 = -0.0182f, m21 = -0.1006f, m22 = 1.1187f;
    const float lumaR = (float)kBt2020LumaR;
    const float lumaG = (float)kBt2020LumaG;
    const float lumaB = (float)kBt2020LumaB;
    const float outScale = kHdrLutInvMaxLinear;

    // Row worker. Called on the calling thread for the last chunk and on
    // background threads for the rest.
    // The out_y coordinate spans [0, outH); we sample the source at
    // out_y * step_y to compensate for any half-res downscale.
    auto process_rows = [&](int y_start, int y_end) {
        for (int out_y = y_start; out_y < y_end; ++out_y) {
            int y = out_y * step_y;
            if (y >= H) y = H - 1;
            uint16_t* out_row = reinterpret_cast<uint16_t*>(
                                    rgb->data[0] + (size_t)out_y * rgb->linesize[0]);
            uint16_t* Y_row = reinterpret_cast<uint16_t*>(
                                    src->data[0] + (size_t)y * src->linesize[0]);
            int cy = y / 2;
            uint16_t* U_row = nullptr;
            uint16_t* V_row = nullptr;
            uint16_t* UV_row = nullptr;
            if (is_p010) {
                UV_row = reinterpret_cast<uint16_t*>(
                            src->data[1] + (size_t)cy * src->linesize[1]);
            } else {
                U_row = reinterpret_cast<uint16_t*>(
                            src->data[1] + (size_t)cy * src->linesize[1]);
                V_row = reinterpret_cast<uint16_t*>(
                            src->data[2] + (size_t)cy * src->linesize[2]);
            }

            for (int out_x = 0; out_x < outW; ++out_x) {
                int x = out_x * step_x;
                if (x >= W) x = W - 1;
                int Yc = (int)(Y_row[x] >> shift);
                int cx = x / 2;
                int Uc, Vc;
                if (is_p010) {
                    Uc = (int)(UV_row[cx * 2    ] >> shift);
                    Vc = (int)(UV_row[cx * 2 + 1] >> shift);
                } else {
                    Uc = (int)U_row[cx];
                    Vc = (int)V_row[cx];
                }

                float Yn = ((float)Yc - y_black)    * inv_y_scale;
                float Un = ((float)Uc - uv_neutral) * inv_uv_scale;
                float Vn = ((float)Vc - uv_neutral) * inv_uv_scale;

                // BT.2020 NCL YUV -> gamma-encoded R'G'B' in BT.2020.
                float Rp = Yn + 1.4746f  * Vn;
                float Gp = Yn - 0.16455f * Un - 0.57135f * Vn;
                float Bp = Yn + 1.8814f  * Un;
                if (Rp < 0.0f) Rp = 0.0f; else if (Rp > 1.0f) Rp = 1.0f;
                if (Gp < 0.0f) Gp = 0.0f; else if (Gp > 1.0f) Gp = 1.0f;
                if (Bp < 0.0f) Bp = 0.0f; else if (Bp > 1.0f) Bp = 1.0f;

                // Inverse EOTF (PQ) or inverse OETF (HLG) via LUT.
                int ri = (int)(Rp * 65535.0f + 0.5f);
                int gi = (int)(Gp * 65535.0f + 0.5f);
                int bi = (int)(Bp * 65535.0f + 0.5f);
                float R = in_lut[ri];
                float G = in_lut[gi];
                float B = in_lut[bi];

                if (is_hlg) {
                    float Ys = lumaR*R + lumaG*G + lumaB*B;
                    if (Ys < 0.0f) Ys = 0.0f; else if (Ys > 1.0f) Ys = 1.0f;
                    int   yi = (int)(Ys * 65535.0f + 0.5f);
                    float gain = hlg_gainlut[yi];
                    R *= gain;
                    G *= gain;
                    B *= gain;
                }

                // Linear-light BT.2020 -> BT.709 gamut.
                float R7 = m00*R + m01*G + m02*B;
                float G7 = m10*R + m11*G + m12*B;
                float B7 = m20*R + m21*G + m22*B;
                if (R7 < 0.0f) R7 = 0.0f;
                if (G7 < 0.0f) G7 = 0.0f;
                if (B7 < 0.0f) B7 = 0.0f;

                // Fused Hable + BT.709 OETF via out_lut.
                float rf = R7 * outScale;  if (rf > 65535.0f) rf = 65535.0f;
                float gf = G7 * outScale;  if (gf > 65535.0f) gf = 65535.0f;
                float bf = B7 * outScale;  if (bf > 65535.0f) bf = 65535.0f;
                out_row[out_x * 3 + 0] = out_lut[(int)(rf + 0.5f)];
                out_row[out_x * 3 + 1] = out_lut[(int)(gf + 0.5f)];
                out_row[out_x * 3 + 2] = out_lut[(int)(bf + 0.5f)];
            }
        }
    };

    // Multithread across row chunks. Half the reported logical cores
    // - the "spare cores for decode / audio / UI" split from the
    // original design. Bumping this higher during a stutter-chase
    // turned out to be irrelevant (the real issue was I/O buffering)
    // and can even hurt on machines where hyperthreading confuses
    // std::thread::hardware_concurrency, so it's back to the original.
    unsigned hw = std::thread::hardware_concurrency();
    int nthreads = (int)std::max(1u, hw / 2);
    if (nthreads > outH) nthreads = outH;
    if (nthreads <= 1) {
        process_rows(0, outH);
    } else {
        std::vector<std::thread> workers;
        workers.reserve(nthreads - 1);
        for (int t = 0; t < nthreads - 1; ++t) {
            int ys = (int)((int64_t)t * outH / nthreads);
            int ye = (int)((int64_t)(t + 1) * outH / nthreads);
            workers.emplace_back(process_rows, ys, ye);
        }
        // Run the last chunk on the calling thread to save a join.
        int ys = (int)((int64_t)(nthreads - 1) * outH / nthreads);
        process_rows(ys, outH);
        for (auto& w : workers) w.join();
    }

    // RGB48LE (SDR BT.709, gamma-encoded, full-range) -> YUV420P (limited).
    // Output size = outW/outH so half-res mode ships a smaller YUV
    // texture; SDL upscales during render. sws is a 1:1 convert here
    // (RGB48 is already at outW/outH from the pixel loop).
    AVFrame* out = av_frame_alloc();
    if (!out) { av_frame_free(&rgb); if (tmp_sw) av_frame_free(&tmp_sw); return nullptr; }
    out->format = AV_PIX_FMT_YUV420P;
    out->width  = outW;
    out->height = outH;
    if (av_frame_get_buffer(out, 32) != 0) {
        av_frame_free(&out);
        av_frame_free(&rgb);
        if (tmp_sw) av_frame_free(&tmp_sw);
        return nullptr;
    }
    sws_hdr_from_rgb_ = sws_getCachedContext(sws_hdr_from_rgb_,
        outW, outH, AV_PIX_FMT_RGB48LE,
        outW, outH, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_hdr_from_rgb_) {
        av_frame_free(&out);
        av_frame_free(&rgb);
        if (tmp_sw) av_frame_free(&tmp_sw);
        return nullptr;
    }
    sws_scale(sws_hdr_from_rgb_, rgb->data, rgb->linesize, 0,
              outH, out->data, out->linesize);

    av_frame_free(&rgb);
    if (tmp_sw) av_frame_free(&tmp_sw);
    return out;
}

void VideoPlayer::flushQueues() {
    {
        std::lock_guard<std::mutex> lk(video_queue_mutex_);
        while (!video_queue_.empty()) {
            AVFrame* f = video_queue_.front().frame;
            av_frame_free(&f);
            video_queue_.pop_front();
        }
        video_queue_cv_.notify_all();
    }
    if (audio_stream_) SDL_ClearAudioStream(audio_stream_);
}

std::vector<VideoPlayer::Chapter> VideoPlayer::getChapters() const {
    std::vector<Chapter> out;
    if (!fmt_ctx_ || fmt_ctx_->nb_chapters == 0) return out;
    out.reserve(fmt_ctx_->nb_chapters);
    for (unsigned int i = 0; i < fmt_ctx_->nb_chapters; ++i) {
        const AVChapter* c = fmt_ctx_->chapters[i];
        if (!c) continue;
        Chapter ch;
        double tb = av_q2d(c->time_base);
        ch.start = (double)c->start * tb;
        ch.end   = (double)c->end   * tb;
        if (c->metadata) {
            AVDictionaryEntry* t = av_dict_get(c->metadata, "title", nullptr, 0);
            if (t && t->value) ch.title = t->value;
        }
        out.push_back(std::move(ch));
    }
    return out;
}

int VideoPlayer::getAudioTrackCount() const {
    if (!fmt_ctx_) return 0;
    int n = 0;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++n;
    }
    return n;
}

int VideoPlayer::getCurrentAudioTrackIndex() const {
    if (!fmt_ctx_ || audio_stream_idx_ < 0) return -1;
    int n = 0;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if ((int)i == audio_stream_idx_) return n;
            ++n;
        }
    }
    return -1;
}

std::string VideoPlayer::getAudioTrackName(int track_idx) const {
    if (!fmt_ctx_) return {};
    int n = 0;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream* st = fmt_ctx_->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (n == track_idx) {
            char buf[256];
            const AVDictionaryEntry* lang  = av_dict_get(st->metadata, "language", nullptr, 0);
            const AVDictionaryEntry* title = av_dict_get(st->metadata, "title", nullptr, 0);
            const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
            const char* codec_name = codec ? codec->name : "unknown";
            int ch = st->codecpar->ch_layout.nb_channels;
            std::snprintf(buf, sizeof(buf), "Track %d - %s%s%s%s%s [%s, %d ch, %d Hz]",
                          n + 1,
                          title ? title->value : "",
                          title ? " " : "",
                          lang ? "(" : "",
                          lang ? lang->value : "",
                          lang ? ")" : "",
                          codec_name, ch, st->codecpar->sample_rate);
            return buf;
        }
        ++n;
    }
    return {};
}

std::string VideoPlayer::getCurrentAudioLanguage() const {
    if (!fmt_ctx_ || audio_stream_idx_ < 0
        || audio_stream_idx_ >= (int)fmt_ctx_->nb_streams) return {};
    AVStream* st = fmt_ctx_->streams[audio_stream_idx_];
    if (!st) return {};
    const AVDictionaryEntry* lang = av_dict_get(st->metadata, "language", nullptr, 0);
    if (!lang || !lang->value) return {};
    std::string out = lang->value;
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return out;
}

bool VideoPlayer::setAudioTrack(int track_idx) {
    if (!loaded_ || !fmt_ctx_) return false;
    int stream_idx = -1;
    int n = 0;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (n == track_idx) { stream_idx = (int)i; break; }
            ++n;
        }
    }
    if (stream_idx < 0) return false;
    if (stream_idx == audio_stream_idx_) return true;

    std::string f = filename_;
    double t = getCurrentTime();
    bool was_playing = playing_.load();
    float vol = volume_.load();
    bool m = muted_.load();

    unload();
    if (!loadFile(f, stream_idx)) return false;
    setVolume(vol);
    setMuted(m);
    seek(t);
    if (was_playing) play();
    return true;
}

// ---------- Embedded subtitle streams ----------

std::vector<VideoPlayer::EmbeddedSubtitleTrack>
VideoPlayer::getEmbeddedSubtitleTracks() const {
    return embedded_sub_tracks_;
}

// Strip ASS/SSA style tags from a dialogue line to get plain display
// text. AV_CODEC_ID_ASS output format is:
//   <ReadOrder>,<Layer>,<Style>,<Name>,<MarginL>,<MarginR>,<MarginV>,
//   <Effect>,<Text>
// Text may contain {tag}...{tag} groups and \N line breaks. Strip
// everything inside braces and translate \N -> newline.
static std::string stripAssTags(const std::string& s) {
    // Skip past the first 8 commas to reach the Text field.
    size_t p = 0;
    int commas = 0;
    while (p < s.size() && commas < 8) {
        if (s[p] == ',') ++commas;
        ++p;
    }
    std::string out;
    out.reserve(s.size() - p);
    bool in_tag = false;
    for (; p < s.size(); ++p) {
        char c = s[p];
        if (c == '{') { in_tag = true; continue; }
        if (c == '}') { in_tag = false; continue; }
        if (in_tag) continue;
        if (c == '\\' && p + 1 < s.size()
            && (s[p+1] == 'N' || s[p+1] == 'n')) {
            out.push_back('\n');
            ++p;
            continue;
        }
        out.push_back(c);
    }
    // Trim trailing CR/LF/whitespace so multi-line cues render cleanly.
    while (!out.empty()
           && (out.back() == '\r' || out.back() == '\n'
               || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

void VideoPlayer::openEmbeddedSubtitleDecoder_(int stream_idx) {
    closeEmbeddedSubtitleDecoder_();
    if (!fmt_ctx_ || stream_idx < 0 || stream_idx >= (int)fmt_ctx_->nb_streams) return;
    AVStream* st = fmt_ctx_->streams[stream_idx];
    if (!st || st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) return;
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return;
    sub_dec_ctx_ = avcodec_alloc_context3(codec);
    if (!sub_dec_ctx_) return;
    if (avcodec_parameters_to_context(sub_dec_ctx_, st->codecpar) < 0
        || avcodec_open2(sub_dec_ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&sub_dec_ctx_);
        sub_dec_ctx_ = nullptr;
        return;
    }
    embedded_sub_stream_idx_ = stream_idx;
    {
        std::lock_guard<std::mutex> lk(sub_cues_mutex_);
        sub_cues_.clear();
    }
}

void VideoPlayer::closeEmbeddedSubtitleDecoder_() {
    if (sub_dec_ctx_) avcodec_free_context(&sub_dec_ctx_);
    sub_dec_ctx_ = nullptr;
    embedded_sub_stream_idx_ = -1;
    {
        std::lock_guard<std::mutex> lk(sub_cues_mutex_);
        sub_cues_.clear();
        // Bitmap cues carry their own RGBA buffers - previously we
        // only cleared text cues here, which left stale PGS/DVB
        // bitmap subs from the just-closed track alive in memory.
        // Two symptoms: (1) getEmbeddedBitmapSubtitleAt returned old
        // cues on the new track's timeline until the 500-cap trim
        // fired, (2) tens of MB of RGBA data leaked per track switch.
        sub_bitmap_cues_.clear();
    }
}

bool VideoPlayer::setActiveEmbeddedSubtitle(int track_index) {
    if (track_index < 0) {
        closeEmbeddedSubtitleDecoder_();
        active_embedded_sub_.store(-1);
        return true;
    }
    if (track_index >= (int)embedded_sub_tracks_.size()) return false;
    const EmbeddedSubtitleTrack& t = embedded_sub_tracks_[track_index];
    if (!t.selectable) return false;
    openEmbeddedSubtitleDecoder_(t.stream_idx);
    if (!sub_dec_ctx_) return false;
    active_embedded_sub_.store(track_index);
    return true;
}

std::string VideoPlayer::getEmbeddedSubtitleAt(double t) const {
    std::lock_guard<std::mutex> lk(sub_cues_mutex_);
    // Linear scan is fine - a two-hour movie has ~1500 cues; iterating
    // once per frame at 30 fps is ~45 k comparisons/s, cheap. Newer
    // cues are appended in-order so we could bisect, but the cost is
    // dwarfed by the tone-map / render passes above us.
    for (const auto& c : sub_cues_) {
        if (t >= c.start && t <= c.end) return c.text;
    }
    return {};
}

void VideoPlayer::handleSubtitlePacket_(AVPacket* pkt) {
    if (!sub_dec_ctx_ || !pkt) return;
    AVSubtitle sub{};
    int got = 0;
    int ret = avcodec_decode_subtitle2(sub_dec_ctx_, &sub, &got, pkt);
    if (ret < 0 || !got) { avsubtitle_free(&sub); return; }

    AVStream* st = fmt_ctx_->streams[embedded_sub_stream_idx_];
    double tb = av_q2d(st->time_base);
    double base_pts = (pkt->pts != AV_NOPTS_VALUE) ? (pkt->pts * tb) : 0.0;
    // sub.start_display_time / end_display_time are milliseconds from
    // the packet pts (per AVSubtitle doc). end_display_time == 0 means
    // "until next cue" so we fall back to +5 s of visibility.
    double start = base_pts + sub.start_display_time / 1000.0;
    double end   = base_pts + sub.end_display_time   / 1000.0;
    if (sub.end_display_time == 0) end = start + 5.0;
    if (pkt->duration > 0) {
        double dur_sec = pkt->duration * tb;
        if (end - start > dur_sec + 0.001) end = start + dur_sec;
    }

    std::string text;
    std::vector<SubBitmapCue> new_bitmaps;
    for (unsigned r = 0; r < sub.num_rects; ++r) {
        AVSubtitleRect* rect = sub.rects[r];
        if (!rect) continue;
        // Text / ASS variants: strip tags and concat.
        if (rect->ass && rect->ass[0]) {
            if (!text.empty()) text.push_back('\n');
            text += stripAssTags(rect->ass);
            continue;
        }
        if (rect->text && rect->text[0]) {
            if (!text.empty()) text.push_back('\n');
            text += rect->text;
            continue;
        }
        // Bitmap: PAL8 pixels in rect->data[0], palette (4 bytes per
        // entry, RGBA order in FFmpeg's convention) in rect->data[1].
        // Convert to ABGR32 (SDL_PIXELFORMAT_ABGR8888) so the renderer
        // can upload directly.
        if (rect->type == SUBTITLE_BITMAP
            && rect->w > 0 && rect->h > 0
            && rect->data[0] && rect->data[1]) {
            SubBitmapCue bc;
            bc.id = sub_bitmap_next_id_++;
            bc.start = start;
            bc.end   = end;
            bc.w = rect->w;
            bc.h = rect->h;
            bc.x = rect->x;
            bc.y = rect->y;
            bc.video_w = video_width_;
            bc.video_h = video_height_;
            bc.rgba.resize((size_t)rect->w * rect->h);
            const uint8_t* pal = rect->data[1];
            for (int py = 0; py < rect->h; ++py) {
                const uint8_t* srow = rect->data[0] + (size_t)py * rect->linesize[0];
                uint32_t* drow = bc.rgba.data() + (size_t)py * rect->w;
                for (int px = 0; px < rect->w; ++px) {
                    uint8_t idx = srow[px];
                    const uint8_t* c = pal + (size_t)idx * 4;
                    // FFmpeg palette layout is RGBA (R,G,B,A). SDL's
                    // ABGR32 in memory is (R,G,B,A) little-endian
                    // matching bytewise, so no swap needed.
                    drow[px] = ((uint32_t)c[3] << 24)   // A -> high
                             | ((uint32_t)c[2] << 16)   // B
                             | ((uint32_t)c[1] << 8)    // G
                             |  (uint32_t)c[0];         // R -> low
                }
            }
            new_bitmaps.push_back(std::move(bc));
        }
    }
    avsubtitle_free(&sub);

    std::lock_guard<std::mutex> lk(sub_cues_mutex_);
    if (!text.empty()) {
        sub_cues_.push_back(SubCue{start, end, std::move(text)});
        if (sub_cues_.size() > 8000) {
            sub_cues_.erase(sub_cues_.begin(), sub_cues_.begin() + 2000);
        }
    }
    for (auto& bc : new_bitmaps) sub_bitmap_cues_.push_back(std::move(bc));
    // Bitmap cues are much heavier (each carries its own RGBA buffer);
    // trim more aggressively.
    if (sub_bitmap_cues_.size() > 500) {
        sub_bitmap_cues_.erase(sub_bitmap_cues_.begin(),
                                sub_bitmap_cues_.begin() + 200);
    }
}

VideoPlayer::BitmapSubtitleView
VideoPlayer::getEmbeddedBitmapSubtitleAt(double t) const {
    std::lock_guard<std::mutex> lk(sub_cues_mutex_);
    for (const auto& c : sub_bitmap_cues_) {
        if (t >= c.start && t <= c.end) {
            BitmapSubtitleView v;
            v.id      = c.id;
            v.w       = c.w;
            v.h       = c.h;
            v.x       = c.x;
            v.y       = c.y;
            v.video_w = c.video_w;
            v.video_h = c.video_h;
            v.rgba    = c.rgba;
            return v;
        }
    }
    return BitmapSubtitleView{};
}

bool VideoPlayer::getLastFrameBGR(int& width, int& height, int& stride,
                                  std::vector<uint8_t>& bgr) {
    AVFrame* src = nullptr;
    {
        std::lock_guard<std::mutex> lk(last_frame_mutex_);
        if (!last_frame_) return false;
        src = av_frame_clone(last_frame_);
    }
    if (!src) return false;
    width  = src->width;
    height = src->height;
    // Windows DIB rows are always DWORD-aligned. Even though top-down
    // isn't strictly required, keeping the stride aligned lets other
    // apps that expect standard DIB layout paste without surprises.
    stride = ((width * 3 + 3) / 4) * 4;
    SwsContext* ctx = sws_getContext(width, height, (AVPixelFormat)src->format,
                                     width, height, AV_PIX_FMT_BGR24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx) { av_frame_free(&src); return false; }
    bgr.assign((size_t)stride * height, 0);
    uint8_t* dst_data[1]   = { bgr.data() };
    int      dst_stride[1] = { stride };
    sws_scale(ctx, src->data, src->linesize, 0, height, dst_data, dst_stride);
    sws_freeContext(ctx);
    av_frame_free(&src);
    return true;
}

bool VideoPlayer::snapshot(const std::string& out_path) {
    AVFrame* src = nullptr;
    {
        std::lock_guard<std::mutex> lk(last_frame_mutex_);
        if (!last_frame_) return false;
        src = av_frame_clone(last_frame_);
    }
    if (!src) return false;

    int W = src->width;
    int H = src->height;

    SwsContext* rgb_ctx = sws_getContext(W, H, (AVPixelFormat)src->format,
                                          W, H, AV_PIX_FMT_RGB24,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!rgb_ctx) { av_frame_free(&src); return false; }

    AVFrame* rgb = av_frame_alloc();
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = W;
    rgb->height = H;
    if (av_frame_get_buffer(rgb, 32) < 0) {
        sws_freeContext(rgb_ctx);
        av_frame_free(&rgb);
        av_frame_free(&src);
        return false;
    }
    sws_scale(rgb_ctx, src->data, src->linesize, 0, H, rgb->data, rgb->linesize);
    sws_freeContext(rgb_ctx);
    av_frame_free(&src);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) { av_frame_free(&rgb); return false; }
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) { av_frame_free(&rgb); return false; }
    enc->width     = W;
    enc->height    = H;
    enc->pix_fmt   = AV_PIX_FMT_RGB24;
    enc->time_base = AVRational{1, 25};

    if (avcodec_open2(enc, codec, nullptr) < 0) {
        avcodec_free_context(&enc);
        av_frame_free(&rgb);
        return false;
    }

    rgb->pts = 0;
    AVPacket* pkt = av_packet_alloc();
    bool ok = false;
    if (avcodec_send_frame(enc, rgb) >= 0) {
        if (avcodec_receive_packet(enc, pkt) >= 0) {
            std::FILE* f = FileIO::open(out_path.c_str(), "wb");
            if (f) {
                if ((int)std::fwrite(pkt->data, 1, pkt->size, f) == pkt->size) ok = true;
                std::fclose(f);
            }
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    av_frame_free(&rgb);
    return ok;
}

bool VideoPlayer::snapshotFromBytes(const std::string& out_path,
                                    int W, int H,
                                    int src_fmt, int stride,
                                    const uint8_t* data) {
    if (!data || W <= 0 || H <= 0 || stride <= 0) return false;
    AVPixelFormat in_fmt = (AVPixelFormat)src_fmt;

    // Wrap caller's buffer in an AVFrame so sws_scale can convert it.
    AVFrame* src = av_frame_alloc();
    if (!src) return false;
    src->format = in_fmt;
    src->width  = W;
    src->height = H;
    src->data[0]     = const_cast<uint8_t*>(data);
    src->linesize[0] = stride;

    SwsContext* ctx = sws_getContext(W, H, in_fmt,
                                     W, H, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx) { av_frame_free(&src); return false; }

    AVFrame* rgb = av_frame_alloc();
    if (!rgb) { sws_freeContext(ctx); av_frame_free(&src); return false; }
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width  = W;
    rgb->height = H;
    if (av_frame_get_buffer(rgb, 32) < 0) {
        sws_freeContext(ctx);
        av_frame_free(&rgb);
        av_frame_free(&src);
        return false;
    }
    sws_scale(ctx, src->data, src->linesize, 0, H, rgb->data, rgb->linesize);
    sws_freeContext(ctx);
    // src just wraps caller's buffer - do NOT free it via av_frame_free
    // (would try to free the borrowed pointer). Detach then release the
    // shell.
    src->data[0]     = nullptr;
    src->linesize[0] = 0;
    av_frame_free(&src);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) { av_frame_free(&rgb); return false; }
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (!enc) { av_frame_free(&rgb); return false; }
    enc->width     = W;
    enc->height    = H;
    enc->pix_fmt   = AV_PIX_FMT_RGB24;
    enc->time_base = AVRational{1, 25};
    if (avcodec_open2(enc, codec, nullptr) < 0) {
        avcodec_free_context(&enc);
        av_frame_free(&rgb);
        return false;
    }
    rgb->pts = 0;
    AVPacket* pkt = av_packet_alloc();
    bool ok = false;
    if (avcodec_send_frame(enc, rgb) >= 0
        && avcodec_receive_packet(enc, pkt) >= 0) {
        std::FILE* f = FileIO::open(out_path.c_str(), "wb");
        if (f) {
            if ((int)std::fwrite(pkt->data, 1, pkt->size, f) == pkt->size) ok = true;
            std::fclose(f);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    av_frame_free(&rgb);
    return ok;
}

// Helper: push a packet clone to a bounded per-stream packet queue.
// Waits (up to shutdown/seek) if the queue is full. Returns true if
// the clone was queued, false if we bailed on quit/seek.
static bool enqueuePacketClone(AVPacket* src,
                               std::deque<AVPacket*>& q,
                               std::mutex& m,
                               std::condition_variable& cv,
                               size_t cap,
                               std::atomic<bool>& quit,
                               std::atomic<bool>& seek_req) {
    AVPacket* clone = av_packet_alloc();
    if (!clone) return false;
    if (av_packet_ref(clone, src) != 0) { av_packet_free(&clone); return false; }
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]() { return q.size() < cap || quit || seek_req; });
    if (quit || seek_req) { av_packet_free(&clone); return false; }
    q.push_back(clone);
    cv.notify_one();
    return true;
}

// Demux thread: reads packets from the AVFormatContext and fans them
// out to per-stream packet queues (video / audio) plus inline
// subtitle handling. Does NO decoding. Decoupling demux from decode
// is required because the audio push path is what nudges MediaClock,
// and MediaClock is what releases video frames - so any coupling
// that lets a full video-frame queue block audio processing creates a
// livelock. See [[mediaclock-seek-pending-timeout]].
void VideoPlayer::demuxDecodeThread() {
    AVPacket* packet = av_packet_alloc();

    while (!quit_) {
        // Atomic exchange claims the request. If the UI sets seek_requested_
        // back to true DURING our seek work (drag-spam or a new arrow-key
        // press mid-recovery), the next loop iteration will pick it up
        // with the freshest seek_target_.
        if (seek_requested_.exchange(false)) {
            video_flush_pending_.store(true);
            double target = seek_target_.load();
            // Arm audio-skip. Audio decode thread will silently discard
            // any decoded frame whose pts is more than 100 ms BEFORE
            // this. Prevents mov chunk-interleaving artifacts from
            // anchoring MediaClock several seconds before the seek
            // target. Cleared inside audio decode when it catches up.
            audio_skip_to_pts_.store(target);
            // Arm video-skip. updateTexture drains any queued frame with
            // pts BEFORE the target instead of releasing it - eliminates
            // the post-seek fast-forward that used to burn the
            // keyframe->target GOP frames at wall-vsync rate. Cleared
            // once a frame at or after target reaches display.
            video_skip_to_pts_.store(target);
            int64_t ts = (int64_t)(target * AV_TIME_BASE);
            open_deadline_.store(SDL_GetTicks() + 5000);
            av_seek_frame(fmt_ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
            open_deadline_.store(0);
            // Discard queued packets on BOTH streams, then flush the
            // decoders under their respective ctx mutexes so the
            // decode threads don't race with the flush.
            {
                std::lock_guard<std::mutex> lk(video_pkt_mutex_);
                while (!video_pkt_queue_.empty()) {
                    AVPacket* p = video_pkt_queue_.front();
                    av_packet_free(&p);
                    video_pkt_queue_.pop_front();
                }
                video_pkt_cv_.notify_all();
            }
            {
                std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
                while (!audio_pkt_queue_.empty()) {
                    AVPacket* p = audio_pkt_queue_.front();
                    av_packet_free(&p);
                    audio_pkt_queue_.pop_front();
                }
                audio_pkt_cv_.notify_all();
            }
            // Drain video output queue BEFORE trying to acquire
            // video_ctx_mutex_. Without this, if the video decode thread
            // is currently blocked in video_queue_cv_.wait (queue full)
            // WHILE holding video_ctx_mutex_, we deadlock: our exchange
            // already cleared seek_requested_ so the decode thread's
            // wait predicate won't fire on it, and updateTexture won't
            // drain the queue while MediaClock is frozen at the seek
            // target. Freeing the queue here gives the decode thread's
            // wait predicate `size < MAX` a true reading, so it wakes,
            // pushes its held frame, and releases the ctx mutex.
            {
                std::lock_guard<std::mutex> lk(video_queue_mutex_);
                while (!video_queue_.empty()) {
                    AVFrame* f = video_queue_.front().frame;
                    av_frame_free(&f);
                    video_queue_.pop_front();
                }
                video_queue_cv_.notify_all();
            }
            {
                std::lock_guard<std::mutex> lk(video_ctx_mutex_);
                if (video_dec_ctx_) avcodec_flush_buffers(video_dec_ctx_);
            }
            {
                std::lock_guard<std::mutex> lk(audio_ctx_mutex_);
                if (audio_dec_ctx_) avcodec_flush_buffers(audio_dec_ctx_);
                if (audio_stream_) {
                    // Same rationale as the first-play dance in play():
                    // on some Windows / SDL3 configurations, once the
                    // device thread has stalled (queued keeps equaling
                    // pushed - SDL playing but not consuming), a plain
                    // SDL_ClearAudioStream leaves it in the same stuck
                    // state after the clear. The user hears silence,
                    // and every audio push nudges MediaClock to the
                    // freshly-captured base pts (== seek target),
                    // pinning video frames until ~5 s of audio piles
                    // up in the stream and demux back-pressure finally
                    // stops the nudge stream (then MediaClock ticks
                    // wall-clock and video moves again). Do the same
                    // Pause -> Delay -> Resume state-edge to force
                    // the device thread to re-latch on our resumed
                    // stream so playback (and audio drain) actually
                    // continue immediately after seek.
                    SDL_ClearAudioStream(audio_stream_);
                    SDL_PauseAudioStreamDevice(audio_stream_);
                    SDL_Delay(20);
                    SDL_ResumeAudioStreamDevice(audio_stream_);
                    // If the user is currently PAUSED (they scrubbed with
                    // the seek bar or pressed Left/Right while paused),
                    // the device is now in the "playing" state and will
                    // consume the fresh audio packets that the decode
                    // thread is about to push - so a short burst of audio
                    // leaks out even though the picture is frozen at the
                    // seek target. Re-pause the device to end the dance
                    // in the state the user actually wants.
                    if (!playing_.load()) {
                        SDL_PauseAudioStreamDevice(audio_stream_);
                    }
                }
            }
            // Second video-output drain: catches any stale frames the
            // video decode thread pushed between the first drain
            // (queue empty) and the avcodec_flush_buffers above
            // (decoder's internal buffer emptied). Those frames are
            // from before the seek and must not reach updateTexture.
            //
            // NOTE: intentionally NOT calling flushQueues() here even
            // though it would do the same drain - flushQueues() also
            // calls SDL_ClearAudioStream, which would UNDO the Pause+
            // Delay+Resume state edge we just performed above and
            // re-stall the device thread. Video-only drain is what
            // this step needs.
            {
                std::lock_guard<std::mutex> lk(video_queue_mutex_);
                while (!video_queue_.empty()) {
                    AVFrame* f = video_queue_.front().frame;
                    av_frame_free(&f);
                    video_queue_.pop_front();
                }
                video_queue_cv_.notify_all();
            }
            // Yadif holds a one-frame reference internally; a seek
            // makes it stale. Force a graph rebuild.
            deinterlace_rebuild_.store(true);
            {
                std::lock_guard<std::mutex> lk(clock_mutex_);
                resetClockLocked(target);
            }
            eof_ = false;
            ended_signaled_ = false;
            // Clear the sticky flush flag AFTER all state is settled
            // so the decode thread's next receive_frame loop starts
            // fresh on the flushed decoder. Notify so the decode
            // thread's queue-wait predicate (which included this flag)
            // re-checks - not strictly required now that the queue is
            // empty, but harmless.
            video_flush_pending_.store(false);
            { std::lock_guard<std::mutex> lk(video_queue_mutex_); video_queue_cv_.notify_all(); }
            continue;
        }

        // Back-pressure: sleep only if audio queue is genuinely full.
        // We do NOT gate on video packet queue size the same way -
        // even if video is backed up, we still want to keep pushing
        // audio packets through so MediaClock keeps getting nudged.
        // The per-stream enqueue below waits if its own queue is
        // full, so memory stays bounded either way.
        if (audio_stream_
            && !demux_ignore_audio_queue_.load(std::memory_order_relaxed)
            && SDL_GetAudioStreamQueued(audio_stream_) > AUDIO_QUEUE_MAX_BYTES) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        open_deadline_.store(SDL_GetTicks() + 5000);
        int ret = av_read_frame(fmt_ctx_, packet);
        open_deadline_.store(0);
        if (ret == AVERROR_EOF || ret == AVERROR(EIO)) {
            // Push flush sentinels (null-data packets) to both decode
            // threads so they drain their internal buffers.
            if (video_dec_ctx_) {
                AVPacket* eos = av_packet_alloc();
                if (eos) {
                    std::lock_guard<std::mutex> lk(video_pkt_mutex_);
                    video_pkt_queue_.push_back(eos);
                    video_pkt_cv_.notify_one();
                }
            }
            if (audio_dec_ctx_ && audio_stream_) {
                AVPacket* eos = av_packet_alloc();
                if (eos) {
                    std::lock_guard<std::mutex> lk(audio_pkt_mutex_);
                    audio_pkt_queue_.push_back(eos);
                    audio_pkt_cv_.notify_one();
                }
            }
            eof_ = true;
            while (!quit_ && !seek_requested_ && eof_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            av_packet_unref(packet);
            continue;
        } else if (ret < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            av_packet_unref(packet);
            continue;
        }

        if (packet->stream_index == video_stream_idx_) {
            if (!demux_discard_video_.load(std::memory_order_relaxed)) {
                enqueuePacketClone(packet, video_pkt_queue_,
                                   video_pkt_mutex_, video_pkt_cv_,
                                   kVideoPktQueueMax, quit_, seek_requested_);
                demux_video_packets_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (packet->stream_index == audio_stream_idx_
                   && audio_dec_ctx_ && swr_ctx_ && audio_stream_) {
            enqueuePacketClone(packet, audio_pkt_queue_,
                               audio_pkt_mutex_, audio_pkt_cv_,
                               kAudioPktQueueMax, quit_, seek_requested_);
        } else if (embedded_sub_stream_idx_ >= 0
                   && packet->stream_index == embedded_sub_stream_idx_
                   && sub_dec_ctx_) {
            handleSubtitlePacket_(packet);
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

// Video decode thread: pulls encoded packets from video_pkt_queue_,
// runs them through the decoder + optional HDR tone-map + optional
// yadif deinterlace, and pushes YUV420P frames to the output
// video_queue_ that updateTexture reads.
void VideoPlayer::videoDecodeThread() {
    AVPacket* pkt = nullptr;
    AVFrame* dec_frame = av_frame_alloc();

    // Local YUV materialization + deinterlace helpers - copied from
    // the old inline code, unchanged semantically. Access to
    // sws_ctx_, filter_graph_, and friends is serialized against
    // demux seek-flush via video_ctx_mutex_.
    auto materializeYUV = [&](AVFrame* src_in) -> AVFrame* {
        if (hdr_kind_ != HdrKind::None
            && hdr_tonemap_enabled_.load(std::memory_order_relaxed)) {
            if (AVFrame* mapped = materializeHdrToneMap_(src_in)) {
                return mapped;
            }
        }
        AVFrame* tmp_sw = nullptr;
        AVFrame* src = src_in;
        if (hw_active_ && src_in->format == hw_pix_fmt_) {
            tmp_sw = av_frame_alloc();
            if (!tmp_sw || av_hwframe_transfer_data(tmp_sw, src_in, 0) < 0) {
                if (tmp_sw) av_frame_free(&tmp_sw);
                return nullptr;
            }
            src = tmp_sw;
        }
        // Output dims come from getOutputWidth/Height so hires-downscale
        // (source > 3840 wide) shrinks the sws_scale target, halving the
        // per-frame CPU->GPU upload and the sws work in one shot. HDR
        // frames were intercepted at the top of this lambda and never
        // reach this path - their own half-res is inside materializeHdrToneMap_.
        const int out_w = getOutputWidth();
        const int out_h = getOutputHeight();
        AVFrame* out = av_frame_alloc();
        if (!out) { if (tmp_sw) av_frame_free(&tmp_sw); return nullptr; }
        out->format = AV_PIX_FMT_YUV420P;
        out->width  = out_w;
        out->height = out_h;
        if (av_frame_get_buffer(out, 32) != 0) {
            av_frame_free(&out);
            if (tmp_sw) av_frame_free(&tmp_sw);
            return nullptr;
        }
        sws_ctx_ = sws_getCachedContext(sws_ctx_,
            src->width, src->height, (AVPixelFormat)src->format,
            out_w, out_h, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            av_frame_free(&out);
            if (tmp_sw) av_frame_free(&tmp_sw);
            return nullptr;
        }
        sws_scale(sws_ctx_, src->data, src->linesize, 0,
                  src->height, out->data, out->linesize);
        if (tmp_sw) av_frame_free(&tmp_sw);
        return out;
    };
    auto materializeWithDeinterlace = [&](AVFrame* in_frame,
                                          double& out_pts) -> AVFrame* {
        out_pts = ptsToSec(in_frame->best_effort_timestamp, video_time_base_);
        DeinterlaceMode dm = deinterlace_mode_.load(std::memory_order_relaxed);
        bool want = (dm == DeinterlaceMode::On)
                 || (dm == DeinterlaceMode::Auto && source_interlaced_);
        bool is_hw = hw_active_ && in_frame->format == hw_pix_fmt_;
        if (!want || is_hw) return materializeYUV(in_frame);
        AVPixelFormat fmt = (AVPixelFormat)in_frame->format;
        bool rebuild = deinterlace_rebuild_.exchange(false);
        if (!filter_graph_
            || filter_in_fmt_ != fmt
            || filter_in_w_   != in_frame->width
            || filter_in_h_   != in_frame->height
            || rebuild) {
            if (!buildDeinterlaceGraph_(fmt, in_frame->width,
                                        in_frame->height,
                                        video_time_base_)) {
                return materializeYUV(in_frame);
            }
        }
        if (av_buffersrc_add_frame(buffersrc_, in_frame) < 0) {
            return materializeYUV(in_frame);
        }
        AVFrame* filt = av_frame_alloc();
        if (!filt) return materializeYUV(in_frame);
        AVFrame* out = nullptr;
        int fret = av_buffersink_get_frame(buffersink_, filt);
        if (fret >= 0) {
            out = materializeYUV(filt);
            out_pts = ptsToSec(filt->best_effort_timestamp, video_time_base_);
            av_frame_unref(filt);
        }
        av_frame_free(&filt);
        if (!out) out = materializeYUV(in_frame);
        return out;
    };

    while (!quit_) {
        {
            std::unique_lock<std::mutex> lk(video_pkt_mutex_);
            video_pkt_cv_.wait(lk, [this]() {
                return !video_pkt_queue_.empty() || quit_;
            });
            if (quit_) break;
            pkt = video_pkt_queue_.front();
            video_pkt_queue_.pop_front();
            video_pkt_cv_.notify_one();
        }
        bool is_flush = (pkt->data == nullptr && pkt->size == 0);
        {
            std::lock_guard<std::mutex> lk(video_ctx_mutex_);
            if (!video_dec_ctx_) { av_packet_free(&pkt); continue; }
            int send_ret = avcodec_send_packet(video_dec_ctx_,
                                    is_flush ? nullptr : pkt);
            video_packets_sent_.fetch_add(1, std::memory_order_relaxed);
            if (send_ret < 0) {
                video_packet_send_errors_.fetch_add(1, std::memory_order_relaxed);
            }
            // Watchdog: if we've been fed a lot of packets and produced
            // ZERO frames, the installed decoder cannot handle this stream
            // (e.g. HEVC Rext / 12-bit against a build without Rext).
            // Latch a flag so:
            //   - UI can show a clear "codec unsupported" OSD
            //   - handleNaturalEnd suppresses the repeat-One replay so we
            //     don't infinite-loop demuxing a file we can't display
            {
                constexpr int kDecoderStuckThreshold = 30;
                int sent = video_packets_sent_.load(std::memory_order_relaxed);
                int made = video_frames_pushed_.load(std::memory_order_relaxed);
                if (sent >= kDecoderStuckThreshold && made == 0
                    && !decoder_stuck_.load(std::memory_order_relaxed)) {
                    decoder_stuck_.store(true, std::memory_order_release);
                    bpProbe("decoder_stuck sent=%d made=0 codec=%s pix=%d hw=%s",
                            sent,
                            video_dec_ctx_ && video_dec_ctx_->codec
                                ? video_dec_ctx_->codec->name : "?",
                            video_dec_ctx_ ? (int)video_dec_ctx_->pix_fmt : -1,
                            hw_name_ ? hw_name_ : "?");
                }
            }
            if (send_ret >= 0) {
                int rframe_ret = 0;
                while (!quit_ && !video_flush_pending_.load()
                       && (rframe_ret = avcodec_receive_frame(video_dec_ctx_, dec_frame)) >= 0) {
                    double pts = 0.0;
                    AVFrame* out = materializeWithDeinterlace(dec_frame, pts);
                    av_frame_unref(dec_frame);
                    if (!out) continue;
                    std::unique_lock<std::mutex> qlk(video_queue_mutex_);
                    // Wait for room, but exit fast on quit / seek / flush
                    // so the demux thread's seek-flush isn't blocked
                    // waiting for us to unstick. seek_requested_ alone
                    // is insufficient because the demux thread's
                    // exchange(false) clears it before starting the
                    // flush - video_flush_pending_ stays sticky through
                    // the whole flush window.
                    video_queue_cv_.wait(qlk, [this]() {
                        return video_queue_.size() < VIDEO_QUEUE_MAX
                            || quit_ || seek_requested_
                            || video_flush_pending_.load();
                    });
                    if (quit_ || seek_requested_ || video_flush_pending_.load()) {
                        av_frame_free(&out);
                        break;
                    }
                    video_queue_.push_back({out, pts});
                    if (!first_video_frame_probed_) {
                        first_video_frame_probed_ = true;
                        bpProbe("first_video_frame pts=%.3f q=%zu", pts, video_queue_.size());
                    }
                    video_frames_pushed_.fetch_add(1, std::memory_order_relaxed);
                }
                if (bpProbeEnabled() && rframe_ret < 0
                    && rframe_ret != AVERROR(EAGAIN) && rframe_ret != AVERROR_EOF) {
                    int rerrs = video_recv_frame_errors_.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (rerrs == 1 || rerrs == 5 || rerrs == 30) {
                        char eb[128] = {};
                        av_strerror(rframe_ret, eb, sizeof(eb));
                        bpProbe("recv_frame err count=%d ret=%d err=\"%s\"",
                                rerrs, rframe_ret, eb);
                    }
                }
            }
        }
        av_packet_free(&pkt);
    }
    av_frame_free(&dec_frame);
}

// Audio decode + push runs on its own thread so a full video queue
// cannot starve audio processing. See the audio_pkt_queue_ /
// [[mediaclock-seek-pending-timeout]] discussion for why this
// separation matters: the audio push path is what nudges MediaClock,
// and MediaClock is what releases video frames from the queue.
// Coupling audio decode to the demux thread (which blocks on
// video-queue-full) causes a deadlock/livelock on any file whose
// demuxer emits a long run of video packets before the first audio
// packet.
void VideoPlayer::audioDecodeThread() {
    AVFrame* dec_frame = av_frame_alloc();
    while (!quit_) {
        AVPacket* pkt = nullptr;
        {
            std::unique_lock<std::mutex> lk(audio_pkt_mutex_);
            audio_pkt_cv_.wait(lk, [this]() {
                return !audio_pkt_queue_.empty() || quit_;
            });
            if (quit_) break;
            pkt = audio_pkt_queue_.front();
            audio_pkt_queue_.pop_front();
            audio_pkt_cv_.notify_one();
        }
        bool is_flush = (pkt->data == nullptr && pkt->size == 0);
        {
            std::lock_guard<std::mutex> lk(audio_ctx_mutex_);
            if (!audio_dec_ctx_ || !swr_ctx_ || !audio_stream_) {
                av_packet_free(&pkt);
                continue;
            }
            int sret = avcodec_send_packet(audio_dec_ctx_,
                                           is_flush ? nullptr : pkt);
            if (sret < 0 && !is_flush) {
                av_packet_free(&pkt);
                continue;
            }
            // Resample + push one audio frame to SDL, run post-seek skip
            // check, and nudge MediaClock. Used for BOTH raw dec_frame
            // (normalizer off) and each filter-output frame from
            // dynaudnorm's buffersink (normalizer on). Extracting this
            // is what fixes the "chik chik" glitch: dynaudnorm buffers
            // ~200 ms of input before emitting each output frame, so
            // pushing dec_frame ALSO on every iteration produced ~2x
            // the audio SDL expected and sounded shredded.
            auto processAndPush = [&](AVFrame* frame) {
                int in_samples = frame->nb_samples;
                int64_t delay = swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate);
                int out_samples = (int)av_rescale_rnd(delay + in_samples,
                                                      audio_sample_rate_,
                                                      audio_dec_ctx_->sample_rate,
                                                      AV_ROUND_UP);
                std::vector<uint8_t> tmp((size_t)out_samples * audio_frame_bytes_);
                uint8_t* out_ptr[1] = { tmp.data() };
                int converted = swr_convert(swr_ctx_, out_ptr, out_samples,
                                            (const uint8_t**)frame->extended_data,
                                            in_samples);
                if (converted <= 0) return;
                int bytes = converted * audio_frame_bytes_;
                {
                    const int16_t* p = (const int16_t*)tmp.data();
                    int nsamp = bytes / 2;
                    int32_t peak = 0;
                    int64_t sqsum = 0;
                    for (int i = 0; i < nsamp; ++i) {
                        int32_t v = p[i]; if (v < 0) v = -v;
                        if (v > peak) peak = v;
                        sqsum += (int64_t)p[i] * (int64_t)p[i];
                    }
                    double rms = nsamp ? std::sqrt((double)sqsum / (double)nsamp) : 0.0;
                    static int pc = 0; ++pc;
                    if (pc <= 40 || pc % 50 == 0) {
                        bpTrace("[push] #%d nb=%d bytes=%d peak=%d rms=%.1f fmt=%d fpts=%.3f",
                                pc, in_samples, bytes, peak, rms,
                                (int)frame->format, ptsToSec(frame->pts, audio_time_base_));
                    }
                }
                double frame_pts_sec = -1.0;
                if (frame->pts != AV_NOPTS_VALUE) {
                    frame_pts_sec = ptsToSec(frame->pts, audio_time_base_);
                }
                // Post-seek audio skip: silently drop frames that
                // land significantly before the seek target. Fixes
                // mov chunk-interleaving pathology where av_seek_frame
                // produces audio packets 3-5 s before the video
                // keyframe (Twitter mp4 with edit list is the
                // canonical case).
                double skip_to = audio_skip_to_pts_.load(std::memory_order_relaxed);
                if (frame_pts_sec >= 0.0
                    && skip_to > -1e17
                    && frame_pts_sec < skip_to - kAudioSkipToleranceSec) {
                    return;
                }
                if (skip_to > -1e17) {
                    audio_skip_to_pts_.store(-1e18, std::memory_order_relaxed);
                }
                samplePeak_((const int16_t*)tmp.data(), bytes);
                {
                    std::lock_guard<std::mutex> lk_tap(audio_tap_mutex_);
                    if (audio_tap_) audio_tap_(tmp.data(), bytes);
                }
                SDL_PutAudioStreamData(audio_stream_, tmp.data(), bytes);
                std::lock_guard<std::mutex> lk2(clock_mutex_);
                if (audio_base_needs_capture_ && frame_pts_sec >= 0.0) {
                    audio_seek_base_time_ = frame_pts_sec;
                    audio_pushed_bytes_ = 0;
                    audio_base_needs_capture_ = false;
                }
                audio_pushed_bytes_ += (uint64_t)bytes;
                int queued = (int)SDL_GetAudioStreamQueued(audio_stream_);
                int64_t played = (int64_t)audio_pushed_bytes_ - queued;
                if (played < 0) played = 0;
                double bps = (double)(audio_frame_bytes_ * audio_sample_rate_);
                if (bps > 0.0) {
                    double heard = audio_seek_base_time_
                                 + (double)played / bps
                                 - (double)audio_out_latency_ms_.load() * 0.001;
                    mediaNudgeLocked_(heard);
                }
            };

            while (!quit_
                   && avcodec_receive_frame(audio_dec_ctx_, dec_frame) >= 0) {
                processAndPush(dec_frame);
                av_frame_unref(dec_frame);
            }
            if (is_flush && audio_stream_) SDL_FlushAudioStream(audio_stream_);
        }
        av_packet_free(&pkt);
    }
    av_frame_free(&dec_frame);
}

bool VideoPlayer::updateTexture(SDL_Texture* texture) {
    if (!loaded_ || !texture) return false;

    // Frame-history walk. If the user has stepped back with `,`, the
    // offset is > 0 and we display the appropriate ring entry without
    // touching the decode pipeline. If somehow the offset points past
    // the ring (shouldn't happen since stepFrame gates on size), snap
    // back to newest and fall through.
    int hist_off = history_offset_.load(std::memory_order_acquire);
    if (hist_off > 0) {
        std::lock_guard<std::mutex> lk(history_mutex_);
        int idx = (int)frame_history_.size() - 1 - hist_off;
        if (idx >= 0) {
            AVFrame* hf = frame_history_[(size_t)idx].frame;
            if (hf) {
                SDL_UpdateYUVTexture(texture, nullptr,
                                     hf->data[0], hf->linesize[0],
                                     hf->data[1], hf->linesize[1],
                                     hf->data[2], hf->linesize[2]);
                // No last_frame_ swap here - the "current" frame in the
                // normal-path sense is still whatever we last released
                // from the queue, and clone bookkeeping stays with the
                // history ring. Also skip EOF detection: we're viewing
                // an older frame, the pipeline is unchanged.
                return true;
            }
        }
        // Off the end of the ring - shouldn't happen, but snap back.
        history_offset_.store(0, std::memory_order_release);
    }

    // Video-side view of the clock. Subtracting av_sync_offset_ has the
    // effect of shifting the video: positive offset -> now_time appears
    // earlier -> video frames released later (video delayed).
    double now_time = getCurrentTime() - av_sync_offset_.load();

    // Deferred-display drain (used by the step-back seek fallback). We
    // silently burn through the queue into the history ring; the FIRST
    // frame we find at or after the target pts becomes the frame the
    // user actually sees. This eliminates the "GOP replay from the
    // keyframe" artifact that would otherwise be visible whenever the
    // ring was exhausted.
    double deferred = deferred_display_pts_.load(std::memory_order_acquire);

    AVFrame* best = nullptr;
    double   best_pts = 0.0;
    bool     video_empty = false;
    {
        std::lock_guard<std::mutex> lk(video_queue_mutex_);
        // Only drop frames that are clearly stale (>200 ms behind the
        // master clock). Kept from the original design so seek recovery
        // works. An earlier revision added a tighter "anti-burst
        // collapse" that fired whenever two consecutive queued frames
        // were both ready; that turned out to cause visible stuttering
        // in its own right - the fix for burst is to make tone-map
        // fast enough that the queue never piles up (half-res HDR),
        // not to drop mid-flight frames.
        while (video_queue_.size() > 1 && video_queue_.front().pts < now_time - 0.2) {
            AVFrame* f = video_queue_.front().frame;
            av_frame_free(&f);
            video_queue_.pop_front();
        }
        // Post-seek pre-target drain. Silently drops keyframe->target
        // frames the decoder produced to reach the seek point so the
        // release stage below doesn't spend its per-vsync budget on
        // frames the user never asked to see. See kVideoSkipToleranceSec
        // doc for the "fast forward after seek" symptom this closes.
        // Cleared once a target-or-later frame is released (below).
        {
            double vskip = video_skip_to_pts_.load(std::memory_order_relaxed);
            if (vskip > -1e17) {
                while (!video_queue_.empty()
                       && video_queue_.front().pts < vskip - kVideoSkipToleranceSec) {
                    AVFrame* f = video_queue_.front().frame;
                    av_frame_free(&f);
                    video_queue_.pop_front();
                    video_queue_cv_.notify_one();
                }
            }
        }
        if (deferred >= 0.0) {
            // Drain toward target. Free all pre-target frames EXCEPT the
            // closest one to target - that one gets displayed as a
            // "preview" while we wait for the actual target frame. This
            // avoids the audio-first-video-late feel: audio recovers
            // fast after a seek, but a 4K decoder can take 100-500 ms to
            // reach the target frame across a long GOP. Without a
            // preview the user sees the PRE-seek frame during that
            // window; with the preview they see something visually
            // close to the target scene.
            // Frames FAR past target (>2 s) are stale - old-position
            // pushes from before the decode thread processed the seek
            // (common on backward seek). Drop them.
            AVFrame* pretarget_best     = nullptr;
            double   pretarget_best_pts = 0.0;
            while (!video_queue_.empty()) {
                AVFrame* f = video_queue_.front().frame;
                double   p = video_queue_.front().pts;
                if (p > deferred + 2.0) {
                    av_frame_free(&f);
                    video_queue_.pop_front();
                    video_queue_cv_.notify_one();
                    continue;
                }
                if (p >= deferred - 0.005) {
                    // At/past target within acceptable window - display.
                    best = f;
                    best_pts = p;
                    video_queue_.pop_front();
                    video_queue_cv_.notify_one();
                    deferred_display_pts_.store(-1.0, std::memory_order_release);
                    // The preview frame (if we picked one) is now
                    // superseded - free it. clearHistory_ ran at seek
                    // entry, so we do NOT push pre-target frames into
                    // the ring: at 4K a long-GOP step-back would push
                    // hundreds of 12 MB frames just to trim them right
                    // back down under the byte cap.
                    if (pretarget_best) av_frame_free(&pretarget_best);
                    break;
                }
                // Pre-target: keep the LATEST (closest-to-target) as
                // preview. Free the earlier candidate.
                if (pretarget_best) av_frame_free(&pretarget_best);
                pretarget_best     = f;
                pretarget_best_pts = p;
                video_queue_.pop_front();
                video_queue_cv_.notify_one();
            }
            // If we didn't reach target this call but did drain some
            // pre-target frames, decide whether to display the closest
            // one as a preview. Keep deferred set so subsequent ticks
            // continue draining until a true target frame arrives.
            //
            // While PLAYING: always show the preview - covers the
            // ~100-500 ms of decoder catch-up on long-GOP 4K so audio
            // doesn't race ahead of visibly-stale video.
            //
            // While PAUSED: only show a preview when it's ALREADY close
            // to the target (within kPausedPreviewWindowSec). This kills
            // the "flash of playback from the seek keyframe up to target"
            // that the user perceives as brief playback, while still
            // giving visual feedback for drag-scrub (where each drag
            // call sets a fresh target that the decoder catches up to
            // quickly, so the pretarget IS near the target and IS shown)
            // AND for the final settle when decoder converges near
            // target after an arrow-key seek. The picture stays on the
            // pre-seek frame during the early drain (frames far from
            // target discarded silently), then updates once for the
            // "almost there" preview, then again for the actual target.
            if (!best && pretarget_best) {
                bool show = playing_.load(std::memory_order_relaxed)
                         || scrubbing_.load(std::memory_order_relaxed);
                if (!show) {
                    // Paused + not scrubbing (arrow-key jump, chapter
                    // click, etc.): keep the picture still during the
                    // early drain, only surface the closest-to-target
                    // frame in the final settle window. Stops the
                    // decoder walk from being visible as a brief burst
                    // of playback ("flash").
                    constexpr double kPausedPreviewWindowSec = 0.5;
                    show = (deferred - pretarget_best_pts)
                            <= kPausedPreviewWindowSec;
                }
                if (show) {
                    best = pretarget_best;
                    best_pts = pretarget_best_pts;
                    // Note: deferred stays set - the drain loop above only
                    // clears it when the actual target is hit.
                } else {
                    av_frame_free(&pretarget_best);
                }
            }
            // If we didn't reach the target this call (queue empty),
            // wait for more frames next tick. Previous frame stays on
            // screen. Don't fall into normal-path release.
        } else if (!video_queue_.empty()) {
            double head_pts = video_queue_.front().pts;
            bool time_ready = head_pts <= now_time + 0.005;
            if (!playing_ && audio_pushed_bytes_ == 0 && head_pts <= now_time + 0.5) {
                time_ready = true;
            }
            // Post-resume kick: release the queued frame immediately if
            // its pts is anywhere near the current clock. Compensates
            // for the pipeline asymmetry where SDL audio becomes
            // audible faster than a video frame reaches the panel;
            // without this the picture visibly follows the sound for
            // one vsync period after every unpause. Tolerance is
            // generous (100 ms) since we're gated on `just_unfroze_`
            // firing only once - subsequent frames go back to the
            // tight 5 ms check.
            if (just_unfroze_.load() && head_pts <= now_time + 0.100) {
                time_ready = true;
                just_unfroze_.store(false);
            }
            if (time_ready) {
                best = video_queue_.front().frame;
                best_pts = head_pts;
                video_queue_.pop_front();
                video_queue_cv_.notify_one();
            }
        }
        // Disarm the post-seek video-skip once a target-or-later frame
        // has actually been released. From here the steady-state release
        // policy takes over. Applies to both the deferred and normal
        // branches - either way, `best_pts` here is at/past vskip because
        // the drop loop above already trimmed everything behind it.
        if (best) {
            double vskip = video_skip_to_pts_.load(std::memory_order_relaxed);
            if (vskip > -1e17 && best_pts >= vskip - kVideoSkipToleranceSec) {
                video_skip_to_pts_.store(-1e18, std::memory_order_relaxed);
            }
        }
        video_empty = video_queue_.empty();
    }

    if (best) {
        SDL_UpdateYUVTexture(texture, nullptr,
                            best->data[0], best->linesize[0],
                            best->data[1], best->linesize[1],
                            best->data[2], best->linesize[2]);
        // Push a clone into the history ring BEFORE handing `best` off to
        // last_frame_ - this is our only opportunity to capture it while
        // we still own the source data. pushHistory_ does the clone +
        // byte-budget trim internally.
        pushHistory_(best, best_pts);
        int nd = video_frames_displayed_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (bpProbeEnabled() && (nd == 1 || nd == 5 || nd == 30 || nd % 60 == 0)) {
            bpProbe("displayed n=%d pts=%.3f clock=%.3f q_after=%zu pushed=%d play=%d",
                    nd, best_pts, now_time,
                    (size_t)video_queue_.size(),
                    video_frames_pushed_.load(std::memory_order_relaxed),
                    playing_ ? 1 : 0);
        }
        std::lock_guard<std::mutex> lk(last_frame_mutex_);
        if (last_frame_) av_frame_free(&last_frame_);
        last_frame_ = best;
    } else if (bpProbeEnabled()) {
        // Every ~1s of no-frame calls, log clock+queue so we can see stalls.
        static thread_local int calls_since_last = 0;
        if (++calls_since_last >= 60) {
            calls_since_last = 0;
            int pushed = video_frames_pushed_.load(std::memory_order_relaxed);
            int shown  = video_frames_displayed_.load(std::memory_order_relaxed);
            bpProbe("stall clock=%.3f q_size=?%s pushed=%d shown=%d play=%d",
                    now_time, video_empty ? "empty" : "nonempty",
                    pushed, shown, playing_ ? 1 : 0);
        }
    }

    // Detect natural end and dispatch the callback (outside all locks).
    if (eof_ && !ended_signaled_) {
        int queued_audio = audio_stream_ ? (int)SDL_GetAudioStreamQueued(audio_stream_) : 0;
        if (video_empty && queued_audio < audio_frame_bytes_ * 512) {
            ended_signaled_ = true;
            playing_ = false;
            if (audio_stream_) SDL_PauseAudioStreamDevice(audio_stream_);
            // Still images hit EOF the instant their single frame decodes.
            // Firing onEnded here would auto-advance the playlist inside
            // a couple of render frames, so a folder-open of PNGs would
            // blink through every image and land on the last one. Skip
            // the callback for stills - the last frame stays uploaded on
            // the SDL texture, so the picture keeps showing and the user
            // advances manually with N.
            if (onEnded && !isStillImage()) onEnded();
        }
    }

    return best != nullptr;
}
