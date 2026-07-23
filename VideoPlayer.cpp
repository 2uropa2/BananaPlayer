#include "VideoPlayer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

static double ptsToSec(int64_t pts, AVRational tb) {
    if (pts == AV_NOPTS_VALUE) return 0.0;
    return pts * av_q2d(tb);
}

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

    if (decode_thread_.joinable()) decode_thread_.join();
    loaded_ = false;

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

    if (swr_ctx_) swr_free(&swr_ctx_);
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (video_dec_ctx_) avcodec_free_context(&video_dec_ctx_);
    if (audio_dec_ctx_) avcodec_free_context(&audio_dec_ctx_);
    if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
    if (fmt_ctx_) avformat_close_input(&fmt_ctx_);

    video_stream_idx_ = -1;
    audio_stream_idx_ = -1;
    video_width_ = 0;
    video_height_ = 0;
    duration_ = 0.0;
    filename_.clear();
    quit_ = false;
    seek_requested_ = false;
    eof_ = false;
    ended_signaled_ = false;
    audio_pushed_bytes_ = 0;
    audio_seek_base_time_ = 0.0;
    base_time_ = 0.0;
    playback_rate_ = 1.0f;
    av_sync_offset_ = 0.0;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    hw_active_  = false;
    hw_name_    = "software";
}

bool VideoPlayer::loadFile(const std::string& filename, int preferred_audio_stream) {
    unload();
    filename_ = filename;

    // Wall-clock deadline for the open+probe path so a stalled network
    // share, a spun-down drive, or a slow URL doesn't freeze the main
    // thread. Deadline is stored in the AVFormatContext via `interrupt_callback`
    // and reset every time we start a fresh open.
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) return false;
    open_deadline_ = SDL_GetTicks() + 5000;   // 5 seconds
    fmt_ctx_->interrupt_callback.callback = &VideoPlayer::openInterruptCb;
    fmt_ctx_->interrupt_callback.opaque   = this;

    if (avformat_open_input(&fmt_ctx_, filename.c_str(), nullptr, nullptr) < 0) {
        unload();
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
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
    if (video_stream_idx_ < 0) { unload(); return false; }

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

    // Video decoder
    {
        AVCodecParameters* par = fmt_ctx_->streams[video_stream_idx_]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) { unload(); return false; }
        video_dec_ctx_ = avcodec_alloc_context3(codec);
        if (!video_dec_ctx_) { unload(); return false; }
        if (avcodec_parameters_to_context(video_dec_ctx_, par) < 0) { unload(); return false; }
        video_dec_ctx_->thread_count = 0;

        // Attempt HW decode BEFORE avcodec_open2 - the codec picks its path
        // when opened. If HW init fails at any level we silently keep SW.
        hw_active_ = tryInitHwDecode(video_dec_ctx_, codec);

        if (avcodec_open2(video_dec_ctx_, codec, nullptr) < 0) {
            // HW-attached open failed. Free the attachment and retry SW.
            if (video_dec_ctx_->hw_device_ctx) av_buffer_unref(&video_dec_ctx_->hw_device_ctx);
            video_dec_ctx_->get_format = nullptr;
            video_dec_ctx_->opaque     = nullptr;
            hw_active_ = false;
            hw_name_   = "software";
            if (avcodec_open2(video_dec_ctx_, codec, nullptr) < 0) {
                unload(); return false;
            }
        }
        video_width_ = video_dec_ctx_->width;
        video_height_ = video_dec_ctx_->height;
        video_time_base_ = fmt_ctx_->streams[video_stream_idx_]->time_base;
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

    // Note: sws_ctx_ is now created lazily during decode via
    // sws_getCachedContext because HW decode paths produce different pixel
    // formats than video_dec_ctx_->pix_fmt (typically NV12 after transfer).

    duration_ = (fmt_ctx_->duration != AV_NOPTS_VALUE)
                    ? (fmt_ctx_->duration / (double)AV_TIME_BASE)
                    : 0.0;

    loaded_ = true;
    quit_ = false;
    eof_ = false;
    ended_signaled_ = false;
    audio_pushed_bytes_ = 0;
    audio_seek_base_time_ = 0.0;
    base_time_ = 0.0;
    start_perf_ = SDL_GetPerformanceCounter();
    first_play_needed_ = true;

    decode_thread_ = std::thread(&VideoPlayer::demuxDecodeThread, this);
    return true;
}

void VideoPlayer::play() {
    if (!loaded_) return;
    if (eof_) seek(0.0);
    if (playing_) return;
    // "First play after load" must NOT be detected by inspecting
    // audio_pushed_bytes_ - decode thread may not have pushed a single
    // byte by the time loadPath calls play() (fast CPU, slow first-packet
    // read), and then we'd skip the un-stick dance and freeze on frame 0.
    // Use the explicit flag set at the end of loadFile.
    bool need_dance = first_play_needed_.exchange(false) && audio_stream_;
    {
        std::lock_guard<std::mutex> lk(clock_mutex_);
        // start_perf_ powers both the wall-clock master and the "audio
        // device is stuck" watchdog inside getMasterClockLocked. Reset
        // it on every play() so both references are fresh.
        start_perf_ = SDL_GetPerformanceCounter();
    }
    ended_signaled_ = false;
    playing_ = true;
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
    }
    playing_ = false;
    if (audio_stream_) SDL_PauseAudioStreamDevice(audio_stream_);
}

void VideoPlayer::togglePause() {
    if (playing_) pause(); else play();
}

void VideoPlayer::seek(double seconds) {
    if (!loaded_) return;
    double d = duration_ > 0.0 ? duration_ : 0.0;
    seconds = std::clamp(seconds, 0.0, d);
    // Any external seek invalidates a still-pending backward-step target.
    // stepFrame() re-sets it AFTER calling seek() so its own use case
    // survives, but a user drag or arrow-key seek shouldn't have to know
    // about the mask.
    step_back_target_.store(-1.0, std::memory_order_release);
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
        // Order matters: seek() clears step_back_target_ (so a plain user
        // seek doesn't get stuck behind a stale mask). Set our target
        // AFTER seek() so this step's mask survives. Any race is < 1 us
        // between the two atomic writes vs a demux/decode round-trip.
        double target = getCurrentTime() + delta;
        seek(target);
        step_back_target_.store(target, std::memory_order_release);
        return;
    }
    // Forward step: cheap - just advance the master clock; updateTexture
    // will pull the next frame from the queue on the next call. Cap the
    // advance at duration - 0.05 so a long hold can't race the clock past
    // the file's end (where updateTexture would freeze because no more
    // frames would ever come). Empty-queue exhaustion during rapid holds
    // is self-healing - the next queued frame gets released next call.
    double cap = (duration_ > 0.0) ? duration_ - 0.050 : 1e18;
    std::lock_guard<std::mutex> lk(clock_mutex_);
    if (audio_stream_idx_ >= 0 && audio_stream_) {
        audio_seek_base_time_ = std::min(audio_seek_base_time_ + delta, cap);
    } else {
        base_time_ = std::min(base_time_ + delta, cap);
    }
}

double VideoPlayer::getCurrentTime() const {
    std::lock_guard<std::mutex> lk(clock_mutex_);
    return getMasterClockLocked();
}

int VideoPlayer::openInterruptCb(void* opaque) {
    auto* self = static_cast<VideoPlayer*>(opaque);
    if (!self || self->open_deadline_ == 0) return 0;
    return (SDL_GetTicks() >= self->open_deadline_) ? 1 : 0;
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
    // start_perf_ is re-anchored for BOTH branches now - the audio-master
    // path uses it too as the reference point for the "audio device is
    // stuck" watchdog inside getMasterClockLocked. Without this the
    // watchdog would keep firing after every seek because wall time
    // would still be counting from the very first play().
    start_perf_ = SDL_GetPerformanceCounter();
    if (audio_stream_idx_ >= 0) {
        audio_pushed_bytes_ = 0;
        audio_seek_base_time_ = t;
    } else {
        base_time_ = t;
    }
}

double VideoPlayer::getMasterClockLocked() const {
    if (audio_stream_idx_ >= 0 && audio_stream_) {
        int queued = (int)SDL_GetAudioStreamQueued(audio_stream_);
        int64_t played = (int64_t)audio_pushed_bytes_ - queued;
        if (played < 0) played = 0;
        double bps = (double)(audio_frame_bytes_ * audio_sample_rate_);
        if (bps <= 0) return audio_seek_base_time_;
        // `played` counts source (input) bytes consumed; the frequency-ratio
        // resampler eats input faster at higher rates, so source-time advances
        // at rate * wall-time automatically. No extra multiplication needed.
        double clock = audio_seek_base_time_ + (double)played / bps;
        // Watchdog: if the audio device stayed stuck (played never
        // grows) and wall time has run far past what the audio clock
        // shows, fall back to wall time so video doesn't freeze on
        // frame 0. Only fires when audio is more than 150 ms behind
        // wall - normal SDL device latency is 20-50 ms, so this
        // threshold ignores healthy latency and only trips when the
        // audio device is genuinely not consuming. The comparison is
        // `wall_clock > clock + 0.15` (not just `> clock`) so healthy
        // audio-master keeps driving the clock; only the pathological
        // "device stuck after Open+Pause+Resume" case falls through.
        // Reset on seek via resetClockLocked which zeroes played and
        // re-anchors audio_seek_base_time_ and start_perf_.
        if (playing_ && start_perf_ != 0) {
            uint64_t now = SDL_GetPerformanceCounter();
            double wall = (double)(now - start_perf_)
                        / (double)SDL_GetPerformanceFrequency();
            double rate = (double)playback_rate_.load();
            double wall_clock = audio_seek_base_time_ + wall * rate;
            if (wall_clock > clock + 0.15) return wall_clock;
        }
        return clock;
    }
    if (!playing_) return base_time_;
    uint64_t now = SDL_GetPerformanceCounter();
    double rate = (double)playback_rate_.load();
    return base_time_ + (double)(now - start_perf_) / (double)SDL_GetPerformanceFrequency() * rate;
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

void VideoPlayer::applyGain() {
    if (audio_stream_) {
        float g = muted_ ? 0.0f : volume_.load();
        SDL_SetAudioStreamGain(audio_stream_, g);
    }
}

void VideoPlayer::setAVSyncOffset(double s) {
    if (s < -1.0) s = -1.0;
    if (s >  1.0) s =  1.0;
    av_sync_offset_.store(s);
}

void VideoPlayer::setPlaybackRate(float r) {
    r = std::clamp(r, 0.25f, 3.0f);
    std::lock_guard<std::mutex> lk(clock_mutex_);
    if (audio_stream_idx_ < 0) {
        // Wall-clock master: re-anchor at current media time under the OLD rate
        // so time doesn't jump when we switch multipliers.
        base_time_ = getMasterClockLocked();
        start_perf_ = SDL_GetPerformanceCounter();
    }
    playback_rate_.store(r);
    if (audio_stream_) {
        SDL_SetAudioStreamFrequencyRatio(audio_stream_, r);
    }
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
            std::FILE* f = std::fopen(out_path.c_str(), "wb");
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

void VideoPlayer::demuxDecodeThread() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* dec_frame = av_frame_alloc();

    while (!quit_) {
        if (seek_requested_) {
            double target = seek_target_.load();
            int64_t ts = (int64_t)(target * AV_TIME_BASE);
            av_seek_frame(fmt_ctx_, -1, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(video_dec_ctx_);
            if (audio_dec_ctx_) avcodec_flush_buffers(audio_dec_ctx_);
            flushQueues();
            {
                std::lock_guard<std::mutex> lk(clock_mutex_);
                resetClockLocked(target);
            }
            eof_ = false;
            ended_signaled_ = false;
            seek_requested_ = false;
            continue;
        }

        {
            std::unique_lock<std::mutex> lk(video_queue_mutex_);
            if (video_queue_.size() >= VIDEO_QUEUE_MAX) {
                video_queue_cv_.wait_for(lk, std::chrono::milliseconds(15));
                continue;
            }
        }
        if (audio_stream_
            && SDL_GetAudioStreamQueued(audio_stream_) > AUDIO_QUEUE_MAX_BYTES) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Local helper: convert a decoded frame (possibly HW-backed) into
        // a YUV420P frame at video_width_ x video_height_. Returns nullptr
        // on failure. `dec_frame` is left in the caller's care.
        auto materializeYUV = [&](AVFrame* src_in) -> AVFrame* {
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
            AVFrame* out = av_frame_alloc();
            if (!out) { if (tmp_sw) av_frame_free(&tmp_sw); return nullptr; }
            out->format = AV_PIX_FMT_YUV420P;
            out->width  = video_width_;
            out->height = video_height_;
            if (av_frame_get_buffer(out, 32) != 0) {
                av_frame_free(&out);
                if (tmp_sw) av_frame_free(&tmp_sw);
                return nullptr;
            }
            sws_ctx_ = sws_getCachedContext(sws_ctx_,
                src->width, src->height, (AVPixelFormat)src->format,
                video_width_, video_height_, AV_PIX_FMT_YUV420P,
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

        int ret = av_read_frame(fmt_ctx_, packet);
        if (ret == AVERROR_EOF || ret == AVERROR(EIO)) {
            avcodec_send_packet(video_dec_ctx_, nullptr);
            while (!quit_ && avcodec_receive_frame(video_dec_ctx_, dec_frame) == 0) {
                AVFrame* out = materializeYUV(dec_frame);
                if (out) {
                    double pts = ptsToSec(dec_frame->best_effort_timestamp, video_time_base_);
                    std::lock_guard<std::mutex> lk(video_queue_mutex_);
                    video_queue_.push_back({out, pts});
                }
                av_frame_unref(dec_frame);
            }
            if (audio_dec_ctx_ && audio_stream_) {
                avcodec_send_packet(audio_dec_ctx_, nullptr);
                while (!quit_ && avcodec_receive_frame(audio_dec_ctx_, dec_frame) == 0) {
                    int in_samples = dec_frame->nb_samples;
                    int64_t delay = swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate);
                    int out_samples = (int)av_rescale_rnd(delay + in_samples,
                                                          audio_sample_rate_,
                                                          audio_dec_ctx_->sample_rate,
                                                          AV_ROUND_UP);
                    std::vector<uint8_t> tmp((size_t)out_samples * audio_frame_bytes_);
                    uint8_t* out_ptr[1] = { tmp.data() };
                    int converted = swr_convert(swr_ctx_, out_ptr, out_samples,
                                                (const uint8_t**)dec_frame->extended_data,
                                                in_samples);
                    if (converted > 0) {
                        int bytes = converted * audio_frame_bytes_;
                        SDL_PutAudioStreamData(audio_stream_, tmp.data(), bytes);
                        std::lock_guard<std::mutex> lk(clock_mutex_);
                        audio_pushed_bytes_ += (uint64_t)bytes;
                    }
                    av_frame_unref(dec_frame);
                }
                SDL_FlushAudioStream(audio_stream_);
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
            if (avcodec_send_packet(video_dec_ctx_, packet) >= 0) {
                while (!quit_ && !seek_requested_
                       && avcodec_receive_frame(video_dec_ctx_, dec_frame) >= 0) {
                    AVFrame* out = materializeYUV(dec_frame);
                    if (!out) { av_frame_unref(dec_frame); continue; }
                    double pts = ptsToSec(dec_frame->best_effort_timestamp, video_time_base_);
                    std::unique_lock<std::mutex> lk(video_queue_mutex_);
                    video_queue_cv_.wait(lk, [this]() {
                        return video_queue_.size() < VIDEO_QUEUE_MAX
                               || quit_ || seek_requested_;
                    });
                    if (quit_ || seek_requested_) {
                        av_frame_free(&out);
                        av_frame_unref(dec_frame);
                        break;
                    }
                    video_queue_.push_back({out, pts});
                    av_frame_unref(dec_frame);
                }
            }
        } else if (packet->stream_index == audio_stream_idx_
                   && audio_dec_ctx_ && swr_ctx_ && audio_stream_) {
            if (avcodec_send_packet(audio_dec_ctx_, packet) >= 0) {
                while (!quit_ && !seek_requested_
                       && avcodec_receive_frame(audio_dec_ctx_, dec_frame) >= 0) {
                    int in_samples = dec_frame->nb_samples;
                    int64_t delay = swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate);
                    int out_samples = (int)av_rescale_rnd(delay + in_samples,
                                                          audio_sample_rate_,
                                                          audio_dec_ctx_->sample_rate,
                                                          AV_ROUND_UP);
                    std::vector<uint8_t> tmp((size_t)out_samples * audio_frame_bytes_);
                    uint8_t* out_ptr[1] = { tmp.data() };
                    int converted = swr_convert(swr_ctx_, out_ptr, out_samples,
                                                (const uint8_t**)dec_frame->extended_data,
                                                in_samples);
                    if (converted > 0) {
                        int bytes = converted * audio_frame_bytes_;
                        SDL_PutAudioStreamData(audio_stream_, tmp.data(), bytes);
                        std::lock_guard<std::mutex> lk(clock_mutex_);
                        audio_pushed_bytes_ += (uint64_t)bytes;
                    }
                    av_frame_unref(dec_frame);
                }
            }
        }
        av_packet_unref(packet);
    }

    av_frame_free(&dec_frame);
    av_packet_free(&packet);
}

bool VideoPlayer::updateTexture(SDL_Texture* texture) {
    if (!loaded_ || !texture) return false;
    // Video-side view of the clock. Subtracting av_sync_offset_ has the
    // effect of shifting the video: positive offset -> now_time appears
    // earlier -> video frames released later (video delayed).
    double now_time = getCurrentTime() - av_sync_offset_.load();

    AVFrame* best = nullptr;
    bool video_empty = false;
    double step_target = step_back_target_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(video_queue_mutex_);
        while (video_queue_.size() > 1 && video_queue_.front().pts < now_time - 0.2) {
            AVFrame* f = video_queue_.front().frame;
            av_frame_free(&f);
            video_queue_.pop_front();
        }
        if (step_target >= 0.0) {
            // Backward step in flight. The pipeline may hold TWO classes
            // of frames the user must NOT see:
            //  a) Stale post-target frames left over from before the seek
            //     was processed by the worker (pts > target + tolerance).
            //  b) Pre-target frames coming out of the current GOP decode
            //     (pts < target - tolerance).
            // Drop both; keep only frames landing in [target-10ms, target+40ms].
            // Release at most one such frame per call, then clear the mask
            // so normal rendering resumes. Do NOT fall into the time_ready
            // branch below - the paused-and-just-seeked shortcut there
            // would happily release any of the stale-post-target frames.
            while (!video_queue_.empty()) {
                double pts = video_queue_.front().pts;
                if (pts < step_target - 0.010 || pts > step_target + 0.040) {
                    AVFrame* f = video_queue_.front().frame;
                    av_frame_free(&f);
                    video_queue_.pop_front();
                    continue;
                }
                // Frame is in the target window - release it.
                best = video_queue_.front().frame;
                video_queue_.pop_front();
                video_queue_cv_.notify_one();
                step_back_target_.store(-1.0, std::memory_order_release);
                break;
            }
            video_empty = video_queue_.empty();
        } else if (!video_queue_.empty()) {
            double head_pts = video_queue_.front().pts;
            bool time_ready = head_pts <= now_time + 0.005;
            if (!playing_ && audio_pushed_bytes_ == 0 && head_pts <= now_time + 0.5) {
                time_ready = true;
            }
            if (time_ready) {
                best = video_queue_.front().frame;
                video_queue_.pop_front();
                video_queue_cv_.notify_one();
            }
            video_empty = video_queue_.empty();
        } else {
            video_empty = true;
        }
    }

    if (best) {
        SDL_UpdateYUVTexture(texture, nullptr,
                            best->data[0], best->linesize[0],
                            best->data[1], best->linesize[1],
                            best->data[2], best->linesize[2]);
        std::lock_guard<std::mutex> lk(last_frame_mutex_);
        if (last_frame_) av_frame_free(&last_frame_);
        last_frame_ = best;
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
