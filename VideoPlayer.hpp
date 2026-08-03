#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libavutil/display.h>
}

#include <SDL3/SDL.h>

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    bool loadFile(const std::string& filename, int preferred_audio_stream = -1);
    void unload();

    void play();
    void pause();
    void togglePause();
    // Regular seek. Any deferred step-back display target is cleared -
    // a user-initiated seek makes an old target meaningless.
    void seek(double seconds) { seekInternal_(seconds, /*preserve_deferred=*/false); }
    void seekRelative(double delta);

    // Advance the display clock without triggering a seek (cheap forward frame step).
    // If delta is negative, falls back to a precise seek.
    void stepFrame(double delta);

    double getCurrentTime() const;
    double getDuration() const { return duration_; }
    bool isPlaying() const { return playing_.load(); }
    bool isLoaded() const { return loaded_.load(); }
    bool isEndOfFile() const { return eof_.load(); }
    bool hasAudio() const { return audio_stream_idx_ >= 0; }
    // True when the current video stream is decoded on the GPU (D3D11VA /
    // DXVA2). Falls back to false when SW decode is in use.
    bool isHardwareDecoding() const { return hw_active_; }
    const char* getHardwareDecoderName() const { return hw_name_; }

    // HDR classification of the source video. When non-None, decoded
    // frames are routed through materializeHdrToneMap_() which does its
    // own YUV->linear RGB conversion (bypassing swscale's BT.2020 path,
    // which is unreliable across FFmpeg versions), applies the inverse
    // EOTF + BT.2020->BT.709 gamut + Hable tone map + BT.709 OETF, and
    // hands SDR RGB48 to swscale for the final YUV420P repack.
    enum class HdrKind { None, PQ, HLG };
    HdrKind getHdrKind() const { return hdr_kind_; }

    // Deinterlacing mode.
    //   Off  - never deinterlace (raw source, comb artifacts on TV rips)
    //   Auto - deinterlace when the codec reports the source as
    //          interlaced (field_order != PROGRESSIVE/UNKNOWN)
    //   On   - always deinterlace regardless of field order (useful when
    //          the field-order tag is wrong)
    // Auto is the default. The filter runs via libavfilter's yadif at
    // "send_frame" (one output per input, no framerate doubling).
    enum class DeinterlaceMode { Off, Auto, On };
    void            setDeinterlaceMode(DeinterlaceMode m);
    DeinterlaceMode getDeinterlaceMode() const { return deinterlace_mode_.load(); }
    // True when the source's field-order metadata indicates interlaced
    // content. Info panel + Auto-mode branch both read this.
    bool isSourceInterlaced() const { return source_interlaced_; }

    // Runtime toggle for the HDR tone-map pass. When false, HDR frames
    // skip the CPU tone-map entirely and fall through to the plain SDR
    // path - useful for viewers who have an HDR monitor and want the
    // source values to reach the display uncorrected.
    void setHdrToneMapEnabled(bool on) { hdr_tonemap_enabled_.store(on); }
    bool isHdrToneMapEnabled() const   { return hdr_tonemap_enabled_.load(); }

    // Half-resolution HDR tone map. Trinary: Off (default) = never;
    // Auto = on for 4K+ HDR only (opt-in perf lever for slow CPUs);
    // ForceOn = always on for any HDR source. Default is Off so users
    // don't silently lose fidelity - the tone-map is fast enough on
    // any modern desktop that halving isn't needed for perf when the
    // pipeline is I/O-buffered properly. Applied at loadFile time.
    // Old config values map cleanly: hdr_half_res=0 -> Off, =1 -> Auto,
    // =2 -> ForceOn (numeric ordering unchanged, just the semantic
    // meaning of 0 flipped from Auto to Off).
    enum class HdrHalfResMode { Off = 0, Auto = 1, ForceOn = 2 };
    void            setHdrHalfResMode(HdrHalfResMode m) { hdr_half_res_mode_.store(m); }
    HdrHalfResMode  getHdrHalfResMode() const           { return hdr_half_res_mode_.load(); }
    // True when the current file is running under half-res tone map
    // (auto or forced). UI reads for the Info panel row.
    bool            isHdrHalfResActive() const          { return hdr_half_res_active_; }
    // Actual output resolution (accounting for any half-res override).
    // Non-HDR files always return video_width_ / video_height_.
    int  getOutputWidth() const;
    int  getOutputHeight() const;
    int getVideoWidth() const { return video_width_; }
    int getVideoHeight() const { return video_height_; }
    const std::string& getFilename() const { return filename_; }

    // Info-panel accessors. Safe to call regardless of load state - each
    // returns a sentinel ("" / 0.0 / 0) when the corresponding stream is
    // absent, so the UI doesn't need to guard every access. Codec names
    // come from `avcodec_get_name` on the active decoder; framerate is
    // taken from the video stream's `r_frame_rate`, falling back to
    // `avg_frame_rate` when the reported "real" rate is 0/1.
    const char* getVideoCodecName() const;
    const char* getAudioCodecName() const;
    double      getVideoFrameRate() const;
    int64_t     getFileBitrate() const;
    int         getAudioSampleRate() const { return audio_stream_idx_ >= 0 ? audio_sample_rate_ : 0; }
    int         getAudioChannels() const   { return audio_stream_idx_ >= 0 ? audio_channels_ : 0; }

    // True when the loaded file is a still-image codec (PNG, JPEG, BMP,
    // TIFF, WebP, ...). The decode pipeline handles these the same way
    // it handles video, but EOF is reached after one frame - callers use
    // this to suppress "advance to next in playlist" so a folder of
    // mixed images and videos doesn't blink past every image in a frame.
    bool isStillImage() const;

    // Container chapters (MKV / MP4 chap0 boxes). Returned in file order
    // with start / end in seconds; title empty when the container didn't
    // set one. Cheap enough to call once per file load - UIController
    // caches the result so the seek-bar draw path doesn't re-walk it.
    struct Chapter {
        double      start = 0.0;
        double      end   = 0.0;
        std::string title;
    };
    std::vector<Chapter> getChapters() const;

    // Clockwise rotation encoded in the container's video display matrix
    // (or the legacy `rotate` metadata as a fallback). 0 / 90 / 180 / 270.
    // Portrait phone videos are typically stored as landscape pixels with
    // a 90-degree rotation tag; PotPlayer/VLC apply it silently. The render loop
    // in main.cpp combines this with the user's Ctrl+R rotation so both
    // stack (intrinsic first, then user).
    int getIntrinsicRotation() const;

    void setVolume(float v);
    float getVolume() const { return volume_.load(); }
    void setMuted(bool m);
    bool isMuted() const { return muted_.load(); }

    // Loudness normalization for quiet content. Runs the FFmpeg
    // `dynaudnorm` audio filter (dynamic loudness normalization) in
    // the decode path. Unlike `loudnorm` proper which has ~3 s of
    // lookahead latency (fine for authoring, breaks A/V sync for
    // real-time playback), dynaudnorm operates on short windows with
    // no lookahead - designed exactly for the "movie has quiet
    // dialogue and I keep reaching for the volume" case.
    // Runs in ADDITION to setNormalizeMode's peak-based static gain -
    // typical use is either/or, but they compose cleanly if both.
    // Persisted in banana.config as `loudness_normalize`.
    void setLoudnessNormalize(bool on);
    bool isLoudnessNormalize() const { return loudness_normalize_.load(); }

    // Per-file audio auto-normalization. When enabled, the decode
    // thread samples the peak sample magnitude across the first few
    // seconds of audio and multiplies a compensating gain into the
    // playback stream so quiet clips come up to a consistent loudness.
    // Off = pass-through; Soft targets peak = 0.90 (headroom ~1 dB);
    // Strong targets peak = 0.98 (headroom ~0.2 dB). Gain is clamped
    // to [1.0, 4.0] so we never *quiet* a source, only boost it.
    enum class NormalizeMode { Off, Soft, Strong };
    void          setNormalizeMode(NormalizeMode m);
    NormalizeMode getNormalizeMode() const { return normalize_mode_.load(); }
    // Current effective auto-gain after the peak analysis has stabilized
    // (1.0 == pass-through). UI reads this for the Info panel row.
    float         getAutoGain() const { return auto_gain_.load(); }

    // Playback rate. 1.0 = real time. Values above/below rescale the master
    // clock and adjust the audio stream's frequency ratio (pitch is not
    // preserved - simple approach). Clamped to [0.25, 3.0].
    void  setPlaybackRate(float r);
    float getPlaybackRate() const { return playback_rate_.load(); }

    // A/V sync offset in seconds. Positive values delay video (use when the
    // audio track is heard AFTER the picture); negative values push video
    // earlier (use when audio is ahead). Applied when the render loop pulls
    // frames off the queue, not to the master clock itself, so seeks and
    // pause math stay unaffected. Clamped to [-1.0, +1.0].
    void   setAVSyncOffset(double s);
    double getAVSyncOffset() const { return av_sync_offset_.load(); }

    // Additional latency between SDL "playing" an audio byte and that
    // byte actually reaching the user's ear via OS audio server + DAC.
    // Subtracted from the audio "heard-now" estimate that MediaClock
    // is nudged toward, so video release lines up with what the user
    // is hearing (not what SDL just handed to the OS).
    // Persisted in banana.config as `audio_out_latency_ms` because it's
    // a hardware property, not per-file. Range [0, 500] ms; default 30
    // matches typical Windows WASAPI shared mode + wired audio. Users
    // with Bluetooth or specific DACs can dial higher.
    void   setAudioOutputLatencyMs(int ms);
    int    getAudioOutputLatencyMs() const { return audio_out_latency_ms_.load(); }

    // Audio track enumeration + switching.
    int getAudioTrackCount() const;
    int getCurrentAudioTrackIndex() const;      // 0-based within audio tracks; -1 if none
    std::string getAudioTrackName(int track_idx) const;
    bool setAudioTrack(int track_idx);
    // ISO 639 language code (from stream metadata) of the CURRENTLY active
    // audio track, lowercase. Empty when no audio or the container didn't
    // set a language tag. Used by the embedded-subtitle auto-picker to
    // skip English subs on Korean-audio content and similar overreach.
    std::string getCurrentAudioLanguage() const;

    // Embedded (container) subtitle streams. Text-format only for now -
    // SRT (`AV_CODEC_ID_SUBRIP`) and ASS/SSA (`AV_CODEC_ID_ASS`,
    // `AV_CODEC_ID_SSA`). Bitmap formats (PGS, DVB, DVD sub) are
    // enumerated but marked non-selectable so the UI can grey them out.
    // A single embedded track can be active at a time; -1 = none (the
    // UIController's external SRT/SMI sidecars are independent).
    struct EmbeddedSubtitleTrack {
        int         stream_idx;    // index into fmt_ctx_->streams
        std::string codec_name;
        std::string language;      // ISO 639, if the container set it
        std::string title;         // human-readable, if set
        bool        selectable;    // false for bitmap formats
    };
    std::vector<EmbeddedSubtitleTrack> getEmbeddedSubtitleTracks() const;
    int  getActiveEmbeddedSubtitle() const { return active_embedded_sub_.load(); }
    // `track_index` is an index INTO getEmbeddedSubtitleTracks(). -1
    // disables. Returns false if the index is out of range or the
    // target track isn't selectable (bitmap).
    bool setActiveEmbeddedSubtitle(int track_index);
    // Look up the currently-displayed embedded cue text at time `t`.
    // Empty string when nothing is on. Thread-safe.
    std::string getEmbeddedSubtitleAt(double t) const;

    // Look up the currently-displayed BITMAP subtitle cue at time `t`.
    // Used for PGS / DVB / DVD bitmap tracks that avcodec_decode_subtitle2
    // returns as SUBTITLE_BITMAP rects. The RGBA buffer is a copy so
    // the caller can hand it off to the renderer without holding our
    // cue mutex. `id` is a monotonically-increasing per-cue serial
    // so the UI can cache a SDL_Texture across frames while the same
    // cue is visible, and rebuild only when the id changes.
    struct BitmapSubtitleView {
        uint64_t id = 0;      // 0 = no cue at this time
        int      w  = 0;
        int      h  = 0;
        int      x  = 0;      // in source-video pixels
        int      y  = 0;
        int      video_w = 0; // source frame width  (for scaling)
        int      video_h = 0; // source frame height (for scaling)
        std::vector<uint32_t> rgba;   // ABGR32 (SDL_PIXELFORMAT_ABGR8888)
    };
    BitmapSubtitleView getEmbeddedBitmapSubtitleAt(double t) const;

    // Save the most recently displayed frame as a PNG at `out_path`.
    bool snapshot(const std::string& out_path);

    // Encode a caller-supplied pixel buffer (top-down) as PNG at
    // `out_path`. Used by UIController's "capture screen" snapshot mode
    // that pulls composited pixels (video + subtitles + HUD) out of the
    // SDL renderer via SDL_RenderReadPixels. `src_fmt` is an AVPixelFormat
    // (e.g. AV_PIX_FMT_RGBA, AV_PIX_FMT_BGRA); `stride` is bytes per row.
    static bool snapshotFromBytes(const std::string& out_path,
                                  int width, int height,
                                  int src_fmt, int stride,
                                  const uint8_t* data);

    // Stream-copy a [start, end] segment of `src` to `dst`. No re-encode:
    // packets are demuxed, rescaled to the output stream time base, and
    // muxed unchanged. That means the output starts at the first keyframe
    // at or before `start` (so it may include a few frames of lead-in),
    // and the container is inferred from the dst extension (.mp4 / .mkv).
    // Blocks the calling thread - callers should show a spinner OSD.
    static bool exportSegment(const std::string& src, const std::string& dst,
                              double start, double end);

    // Encode [start, end] of `src` as an animated GIF at `dst`. Uses
    // libavfilter's `palettegen` + `paletteuse` chain for good color
    // reproduction. `target_fps` caps the GIF frame rate (typical 10-15);
    // `target_width` caps the horizontal size (aspect-preserved). The
    // caller should clamp the segment length; anything much beyond
    // ~30 s at 480 px starts to burn hundreds of MB of RAM inside the
    // palettegen filter.
    static bool exportSegmentAsGif(const std::string& src, const std::string& dst,
                                   double start, double end,
                                   int target_fps, int target_width);

    // Copy the most-recently displayed frame into `bgr` as a tightly-
    // packed top-down BGR24 image with 4-byte-aligned rows. Populates
    // width/height/stride. Used by UIController to push the frame into
    // the Windows clipboard as a CF_DIB. Returns false if no frame has
    // been displayed yet.
    bool getLastFrameBGR(int& width, int& height, int& stride,
                         std::vector<uint8_t>& bgr);

    // Main-thread call. Returns true if the texture was updated with a new frame.
    // Also handles the natural-end detection and dispatches `onEnded` when appropriate.
    bool updateTexture(SDL_Texture* texture);

    // Fired on the main thread once playback naturally reaches EOF and all buffers drain.
    std::function<void()> onEnded;

private:
    void demuxDecodeThread();
    void resetClockLocked(double t);
    double getMasterClockLocked() const;
    void flushQueues();
    void applyGain();

    // Try to set up hardware decode on `codec_ctx` using the given codec.
    // Returns true if hw_device_ctx_ / hw_pix_fmt_ / hw_name_ are populated
    // and the codec context has been prepared to emit HW frames.
    bool tryInitHwDecode(AVCodecContext* codec_ctx, const AVCodec* codec);
    // get_format callback trampoline used by FFmpeg to pick our HW pix fmt.
    static enum AVPixelFormat pickHwPixFormat(AVCodecContext* ctx,
                                              const enum AVPixelFormat* pix_fmts);
    // FFmpeg polls this repeatedly during avformat_open_input /
    // avformat_find_stream_info; returning non-zero tells it to bail out.
    // We use a wall-clock deadline so bad network shares / dead drives
    // don't freeze the UI thread indefinitely.
    static int openInterruptCb(void* opaque);
    uint64_t open_deadline_ = 0;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* video_dec_ctx_ = nullptr;
    AVCodecContext* audio_dec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;

    // Hardware decode state (D3D11VA / DXVA2, in that preference order).
    AVBufferRef*   hw_device_ctx_ = nullptr;
    AVPixelFormat  hw_pix_fmt_    = AV_PIX_FMT_NONE;
    bool           hw_active_     = false;
    const char*    hw_name_       = "software";

    // HDR tone-map pipeline state. `hdr_kind_` is set in loadFile from
    // the stream's color_trc; when non-None, materializeYUV routes
    // decoded frames through materializeHdrToneMap_. The RGB->YUV sws
    // context on the tail end is cached across frames.
    HdrKind           hdr_kind_             = HdrKind::None;
    std::atomic<bool> hdr_tonemap_enabled_{true};
    // User's half-res-HDR preference. `hdr_half_res_active_` is the
    // decision materialised at loadFile time from the mode plus the
    // source dims + HDR-ness. Default Off - see HdrHalfResMode doc.
    std::atomic<HdrHalfResMode> hdr_half_res_mode_{HdrHalfResMode::Off};
    bool                        hdr_half_res_active_ = false;
    // Deinterlacing state. Only touched from the decode thread except
    // for `deinterlace_mode_` and `deinterlace_rebuild_` which flip via
    // the UI. `filter_graph_` etc. are owned by the decode thread and
    // rebuilt whenever the mode or source pixel format changes.
    // Default Off, not Auto: many progressive-encoded files (especially
    // HandBrake output and older MP4s) carry a spurious field_order=TT
    // tag. Auto would then engage yadif on progressive content, which
    // is a delay-1 filter and desyncs video against the audio clock by
    // one frame. Users who need deinterlacing enable it explicitly.
    std::atomic<DeinterlaceMode> deinterlace_mode_{DeinterlaceMode::Off};
    std::atomic<bool>            deinterlace_rebuild_{false};
    bool                         source_interlaced_ = false;
    AVFilterGraph*   filter_graph_ = nullptr;
    AVFilterContext* buffersrc_    = nullptr;
    AVFilterContext* buffersink_   = nullptr;
    AVPixelFormat    filter_in_fmt_ = AV_PIX_FMT_NONE;
    int              filter_in_w_ = 0;
    int              filter_in_h_ = 0;
    AVRational       filter_in_tb_{0, 1};
    // Build (or rebuild) the yadif filter graph for the given input
    // pixel format + size + time base. Returns true when the graph is
    // ready. Called from the decode thread.
    bool buildDeinterlaceGraph_(AVPixelFormat pix_fmt, int W, int H,
                                AVRational time_base);
    void freeDeinterlaceGraph_();
    // Loudness normalize (dynaudnorm) audio filter graph. Owned by the
    // decode thread. `loudness_normalize_` is the UI toggle;
    // `loudness_rebuild_` flips when the toggle changes so the next
    // decode iteration tears down / rebuilds.
    std::atomic<bool>            loudness_normalize_{false};
    std::atomic<bool>            loudness_rebuild_{false};
    // Set true when a graph build attempt failed for the current audio
    // configuration. Prevents the decode thread from spinning on
    // buildLoudnessGraph_() every frame. Cleared on toggle-off / on
    // rebuild request / on format change.
    bool                         loudness_build_failed_ = false;
    AVFilterGraph*   audio_filter_graph_ = nullptr;
    AVFilterContext* audio_buffersrc_    = nullptr;
    AVFilterContext* audio_buffersink_   = nullptr;
    int              audio_filter_rate_     = 0;
    AVSampleFormat   audio_filter_fmt_      = AV_SAMPLE_FMT_NONE;
    uint64_t         audio_filter_ch_hash_  = 0;
    bool buildLoudnessGraph_(int rate, AVSampleFormat fmt,
                             const AVChannelLayout& layout);
    void freeLoudnessGraph_();
    SwsContext*    sws_hdr_from_rgb_  = nullptr;   // RGB48LE (SDR 709) -> YUV420P
    // Precomputed tables that keep the per-pixel hot loop free of pow(),
    // div, and function calls:
    //   in_lut[65536]     = PQ inverse EOTF or HLG inverse OETF, keyed on
    //                       a 16-bit R'/G'/B' code, output is linear light
    //                       (SDR-normalized for PQ, scene light for HLG).
    //   out_lut[65536]    = Hable filmic tone map + BT.709 OETF fused,
    //                       keyed by (linear * kHdrLutInvMaxLinear).
    //   hlg_gain_lut[65536] = HLG OOTF gain (10 * Ys^0.2), keyed on Ys*65535.
    std::vector<float>    hdr_input_lut_;
    std::vector<uint16_t> hdr_out_lut_;
    std::vector<float>    hdr_hlg_gain_lut_;
    void     ensureHdrLuts_();
    // Convert a HDR-tagged decoded frame (possibly HW-backed) into a fully
    // tone-mapped SDR YUV420P frame at video_width_ x video_height_.
    // Returns nullptr on failure; caller falls back to SDR path.
    AVFrame* materializeHdrToneMap_(AVFrame* src_in);

    int video_stream_idx_ = -1;
    int audio_stream_idx_ = -1;
    AVRational video_time_base_{0, 1};
    AVRational audio_time_base_{0, 1};

    int video_width_ = 0;
    int video_height_ = 0;
    double duration_ = 0.0;
    std::string filename_;

    std::atomic<bool> loaded_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> quit_{false};
    std::atomic<bool> seek_requested_{false};
    std::atomic<double> seek_target_{0.0};
    // Frame history ring - a small memory-capped store of the last N
    // decoded YUV frames the player has displayed. Powers seek-less
    // frame stepping in BOTH directions: stepFrame(-delta) walks back,
    // stepFrame(+delta) walks forward. Only when the walk falls off the
    // end of the ring do we resort to a real seek. Byte budget is used
    // (not frame count) so 1080p and 4K clips get consistent history
    // depth in wall-clock terms without ballooning memory on tall clips.
    struct HistoryFrame {
        AVFrame* frame = nullptr;   // owned deep-cloned YUV420P frame
        double   pts   = 0.0;
    };
    std::mutex             history_mutex_;
    std::deque<HistoryFrame> frame_history_;
    // Set by stepFrame(-delta) when the ring is exhausted and we've
    // fallen back to a seek. updateTexture then silently drains queue
    // frames into the ring instead of displaying them, until it reaches
    // a frame at or beyond this pts - which becomes the newly-viewed
    // frame. Without this, the GOP decode after the seek would flash
    // playback from the keyframe forward before landing on target -
    // exactly the "same scene repeats" artifact the user reported.
    std::atomic<double>    deferred_display_pts_{-1.0};
    // Offset from the newest history entry we're viewing.
    //  0 = at newest, playback and normal stepping run untouched
    //  N > 0 = viewing history_[size-1-N], i.e. N frames older
    std::atomic<int>       history_offset_{0};
    // Approx bytes currently held; cap at 128 MB so a 1080p clip carries
    // ~40 frames of history and a 4K clip still gets ~10.
    uint64_t               history_bytes_       = 0;
    static constexpr uint64_t kHistoryBytesCap  = 128ULL * 1024 * 1024;

    // Internal seek with control over the deferred-display target -
    // stepFrame's own step-back seek needs to survive the reset that
    // a user drag or arrow-key seek should trigger.
    void seekInternal_(double seconds, bool preserve_deferred);

    // Push a deep clone of frame into frame_history_ and trim from the
    // front until the byte budget is met. Called from updateTexture on
    // every normal-path frame release.
    void   pushHistory_(AVFrame* frame, double pts);
    // Take ownership of an already-decoded frame (no clone) and put it
    // into frame_history_, trimming the front by byte budget. Used by
    // the deferred-display drain in updateTexture where we're burning
    // through the queue into the ring without a texture upload.
    void   pushHistoryTakingOwnership_(AVFrame* frame, double pts);
    // Free every AVFrame in the history and reset the offset. Called on
    // any external seek, on unload, and when play() resumes from a
    // walked-back position.
    void   clearHistory_();
    std::atomic<bool> eof_{false};
    std::atomic<bool> ended_signaled_{false};
    // Set true at end of loadFile, cleared after the "un-stick audio"
    // dance runs in play(). Independent of audio_pushed_bytes_ because
    // decode may not have pushed anything yet by the time play() is
    // called via loadPath.
    std::atomic<bool> first_play_needed_{false};
    // Set true by play() every time it unfreezes. First updateTexture
    // call after play() force-releases a queued frame if one is
    // available even if MediaClock hasn't advanced quite far enough
    // yet. Compensates for the pipeline asymmetry at resume: SDL
    // audio starts audible in ~20 ms while a fresh video frame needs
    // ~30 ms (vsync + DWM + panel) to reach the eyes. Without this
    // kick the picture visibly lags the sound for one frame after
    // every unpause. Cleared by updateTexture after the kick fires.
    std::atomic<bool> just_unfroze_{false};

    mutable std::mutex clock_mutex_;
    double base_time_ = 0.0;
    uint64_t start_perf_ = 0;
    uint64_t audio_pushed_bytes_ = 0;
    double audio_seek_base_time_ = 0.0;
    // Wall-clock anchored media clock. Independent of any specific
    // renderer's chunking - grows smoothly with perfcounter. Audio
    // decode path calls mediaNudge_() every push with a "heard-now"
    // estimate; small deviations are absorbed into anchor_pts_
    // gradually (5 % of delta per nudge) so playback stays audio-
    // synced long-term without inheriting SDL's chunked-processing
    // jitter frame-to-frame. Video release path calls mediaNow_()
    // for the "current time" it compares frame pts against.
    double   media_anchor_pts_  = 0.0;
    uint64_t media_anchor_perf_ = 0;
    bool     media_paused_      = true;
    // When true, the MediaClock is frozen at anchor_pts_ regardless of
    // wall-clock advance. Set immediately after seek / loadFile so that
    // while the decoder is catching up (typically 50-500 ms for a fresh
    // seek target), the clock doesn't drift ahead of what audio can
    // actually deliver. Cleared by the first audio nudge after the
    // gap, which also hard-anchors to the true audio position. Without
    // this, video appears to fast-forward for the first 1-5 s after
    // every seek as it chases a MediaClock that got a head start.
    bool     media_seek_pending_ = false;
    // When true, next audio frame's PTS becomes audio_seek_base_time_.
    // Set on loadFile and after every seek. Without this the seek base
    // is the seek TARGET (e.g. 0.0), but the first actually-decoded
    // audio frame is at whatever the codec / container start-time is
    // (AAC encoder-delay padding ~43 ms, MP4 start_time offsets, MKV
    // audio track offsets). The delta shows up as a constant per-file
    // lip-sync offset that no global tuning can fix.
    bool     audio_base_needs_capture_ = false;
    double   mediaNowLocked_() const;
    void     mediaAnchorLocked_(double pts);
    void     mediaSetPausedLocked_(bool paused);
    void     mediaNudgeLocked_(double heard_pts);

    struct QueuedVideoFrame { AVFrame* frame; double pts; };
    std::mutex video_queue_mutex_;
    std::condition_variable video_queue_cv_;
    std::deque<QueuedVideoFrame> video_queue_;
    // Enough decoded-frame headroom to ride out a ~500 ms I/O spike
    // on a slow drive without draining. Was 8; bumped to 16 after
    // reports of choppy 4K HDR playback from large files on external
    // HDDs. At 4K half-res (~3 MB per YUV420P frame) 16 slots = 48 MB;
    // at full 4K it's ~192 MB - still fine on modern desktops.
    static constexpr size_t VIDEO_QUEUE_MAX = 16;

    // ~1 second of stereo 48 kHz S16 audio. 256 KB gave ~1.3 s but on
    // slow drives the audio-side flow was fine anyway; keep some head-
    // room so the decode thread doesn't hard-block on the audio queue
    // when video decode is fighting for the same disk read bandwidth.
    static constexpr int AUDIO_QUEUE_MAX_BYTES = 768 * 1024;

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<float> playback_rate_{1.0f};
    std::atomic<double> av_sync_offset_{0.0};
    std::atomic<int>   audio_out_latency_ms_{30};   // see setter comment
    // Embedded subtitle state. Cache populated once per file in
    // loadFile; the decoder is opened lazily on setActiveEmbeddedSubtitle.
    // Decoded cues live in a mutex-protected vector so the UI thread
    // can look them up per-frame without racing the demux thread that
    // fills it. AV_CODEC_ID_ASS output is stripped of ASS style tags
    // before being stored - we render plain text.
    std::vector<EmbeddedSubtitleTrack> embedded_sub_tracks_;
    std::atomic<int>  active_embedded_sub_{-1};
    int               embedded_sub_stream_idx_ = -1;
    AVCodecContext*   sub_dec_ctx_ = nullptr;
    struct SubCue { double start; double end; std::string text; };
    struct SubBitmapCue {
        uint64_t id;
        double   start;
        double   end;
        int      w, h, x, y;
        int      video_w, video_h;
        std::vector<uint32_t> rgba;    // SDL ABGR32
    };
    mutable std::mutex sub_cues_mutex_;
    std::vector<SubCue>       sub_cues_;
    std::vector<SubBitmapCue> sub_bitmap_cues_;
    uint64_t                  sub_bitmap_next_id_ = 1;
    void openEmbeddedSubtitleDecoder_(int stream_idx);
    void closeEmbeddedSubtitleDecoder_();
    void handleSubtitlePacket_(AVPacket* pkt);
    // Auto-normalization state. `auto_gain_` is the multiplier applied
    // in applyGain(); starts at 1.0, gets updated once the peak-analysis
    // window finishes. `peak_bytes_analyzed_` tracks how much has been
    // scanned so far; `peak_max_abs_` is the running max sample magnitude
    // normalized to [0, 1]. Reset in loadFile.
    std::atomic<NormalizeMode> normalize_mode_{NormalizeMode::Off};
    std::atomic<float> auto_gain_{1.0f};
    std::atomic<uint64_t> peak_bytes_analyzed_{0};
    std::atomic<float> peak_max_abs_{0.0f};
    static constexpr uint64_t kPeakAnalysisTargetSec = 4;   // seconds
    void finalizeAutoGain_();
    // Scan freshly-converted S16 stereo samples for peak magnitude.
    // No-op when normalize mode is Off or the analysis window has
    // already been fulfilled. Called from the audio decode path.
    void samplePeak_(const int16_t* samples, int byte_count);

    SDL_AudioStream* audio_stream_ = nullptr;
    int audio_sample_rate_ = 44100;
    int audio_channels_ = 2;
    int audio_frame_bytes_ = 4;

    std::thread decode_thread_;

    // Most-recently-displayed frame, kept for snapshots.
    std::mutex last_frame_mutex_;
    AVFrame* last_frame_ = nullptr;
};
