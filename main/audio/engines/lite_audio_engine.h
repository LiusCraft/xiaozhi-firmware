#ifndef LITE_AUDIO_ENGINE_H
#define LITE_AUDIO_ENGINE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio_engine.h"

class EspWakeWord;
struct aec_handle_t;

class LiteAudioEngine : public AudioEngine {
public:
    LiteAudioEngine();
    ~LiteAudioEngine() override;

    bool Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;

    void EnableWakeWordDetection(bool enable) override;
    void EnableVoiceProcessing(bool enable) override;
    void EnableDeviceAec(bool enable) override;
    // True only when the standalone AEC was actually created (it may be refused
    // when internal RAM is too low).
    bool IsDeviceAecActive();

    bool HasWakeWord() const override;
    bool IsWakeWordDetectionEnabled() const override;
    bool IsVoiceProcessingEnabled() const override;
    bool IsAfeWakeWord() const override { return false; }
    size_t GetFeedSize() const override;

    // Release the standalone WakeNet allocation while it cannot be used, then
    // recreate it before wake word detection is enabled again.
    void ReleaseWakeWordResources();
    bool RestoreWakeWordResources();

    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;

    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override;

private:
    AudioCodec* codec_ = nullptr;
    std::unique_ptr<EspWakeWord> wake_word_;
    srmodel_list_t* models_list_ = nullptr;
    bool should_have_wake_word_ = false;
    // WakeNet and AEC do not fit in internal RAM together on ESP32-C3, so the
    // wake word is unloaded while AEC is active (realtime mode).
    bool wake_word_suppressed_ = false;
    std::atomic<bool> wake_word_enabled_ = false;
    std::atomic<bool> voice_processing_enabled_ = false;
    int frame_samples_ = 0;
    std::vector<int16_t> output_buffer_;
    std::mutex output_mutex_;
    mutable std::mutex wake_word_mutex_;

    std::function<void(const std::string&)> wake_word_detected_callback_;
    std::function<void(std::vector<int16_t>&&)> output_callback_;
    std::function<void(bool)> vad_state_change_callback_;
    std::string empty_wake_word_;

    // Standalone esp-sr AEC (no AFE). Created only while device AEC is enabled
    // and released when disabled so its memory is available to other modes.
    aec_handle_t* aec_ = nullptr;
    int aec_chunk_samples_ = 0;
    int aec_fill_ = 0;
    int16_t* aec_mic_ = nullptr;
    int16_t* aec_ref_ = nullptr;
    int16_t* aec_out_ = nullptr;
    int64_t aec_time_us_ = 0;
    int aec_frames_ = 0;
    std::mutex aec_mutex_;

    bool CreateWakeWordLocked();
    bool CreateAecLocked();
    void DestroyAecLocked();
    void ProcessAecLocked(const std::vector<int16_t>& data);
    void OutputRawAudio(const std::vector<int16_t>& data);
    void OutputMono(const int16_t* data, size_t samples);
    void FlushOutputLocked();
};

#endif
