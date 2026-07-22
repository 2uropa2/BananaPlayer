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
    void seek(double seconds);
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
    // a 90° rotation tag; PotPlayer/VLC apply it silently. The render loop
    // in main.cpp combines this with the user's Ctrl+R rotation so both
    // stack (intrinsic first, then user).
    int getIntrinsicRotation() const;

    void setVolume(float v);
    float getVolume() const { return volume_.load(); }
    void setMuted(bool m);
    bool isMuted() const { return muted_.load(); }

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

    // Audio track enumeration + switching.
    int getAudioTrackCount() const;
    int getCurrentAudioTrackIndex() const;      // 0-based within audio tracks; -1 if none
    std::string getAudioTrackName(int track_idx) const;
    bool setAudioTrack(int track_idx);

    // Save the most recently displayed frame as a PNG at `out_path`.
    bool snapshot(const std::string& out_path);

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
    std::atomic<bool> eof_{false};
    std::atomic<bool> ended_signaled_{false};
    // Set true at end of loadFile, cleared after the "un-stick audio"
    // dance runs in play(). Independent of audio_pushed_bytes_ because
    // decode may not have pushed anything yet by the time play() is
    // called via loadPath.
    std::atomic<bool> first_play_needed_{false};

    mutable std::mutex clock_mutex_;
    double base_time_ = 0.0;
    uint64_t start_perf_ = 0;
    uint64_t audio_pushed_bytes_ = 0;
    double audio_seek_base_time_ = 0.0;

    struct QueuedVideoFrame { AVFrame* frame; double pts; };
    std::mutex video_queue_mutex_;
    std::condition_variable video_queue_cv_;
    std::deque<QueuedVideoFrame> video_queue_;
    static constexpr size_t VIDEO_QUEUE_MAX = 8;

    static constexpr int AUDIO_QUEUE_MAX_BYTES = 256 * 1024;

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<float> playback_rate_{1.0f};
    std::atomic<double> av_sync_offset_{0.0};

    SDL_AudioStream* audio_stream_ = nullptr;
    int audio_sample_rate_ = 44100;
    int audio_channels_ = 2;
    int audio_frame_bytes_ = 4;

    std::thread decode_thread_;

    // Most-recently-displayed frame, kept for snapshots.
    std::mutex last_frame_mutex_;
    AVFrame* last_frame_ = nullptr;
};
