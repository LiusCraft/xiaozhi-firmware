#include "lite_audio_engine.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wn_models.h>
#if CONFIG_USE_DEVICE_AEC
#include <esp_aec.h>
#endif

#include "wake_words/esp_wake_word.h"

#define TAG "LiteAudioEngine"

#if CONFIG_USE_DEVICE_AEC
// Measured on ESP32-C3 (esp-sr 2.4.7, SR_LOW_COST): aec_create() + buffers take
// about 57 KB of internal RAM. Keep a reserve for Wi-Fi, TLS and Opus so that
// enabling AEC can never starve the rest of the system.
static constexpr size_t kAecHeapEstimate = 58 * 1024;
static constexpr size_t kAecHeapReserve = 40 * 1024;
#endif

LiteAudioEngine::LiteAudioEngine() = default;

LiteAudioEngine::~LiteAudioEngine() {
    std::lock_guard<std::mutex> lock(aec_mutex_);
    DestroyAecLocked();
}

bool LiteAudioEngine::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    models_list_ = models_list;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);

    should_have_wake_word_ = models_list != nullptr &&
        esp_srmodel_filter(models_list, ESP_WN_PREFIX, nullptr) != nullptr;
#if CONFIG_USE_ESP_WAKE_WORD
    should_have_wake_word_ = should_have_wake_word_ || models_list == nullptr;
#endif
    {
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        if (!CreateWakeWordLocked()) {
            return false;
        }
    }

    ESP_LOGI(TAG, "Initialized, WakeNet: %s", wake_word_ ? "yes" : "no");
    return true;
}

void LiteAudioEngine::Feed(std::vector<int16_t>&& data) {
    {
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        if (wake_word_enabled_ && wake_word_) {
            wake_word_->Feed(data);
        }
    }
    if (voice_processing_enabled_) {
        std::lock_guard<std::mutex> lock(aec_mutex_);
        if (aec_ != nullptr) {
            ProcessAecLocked(data);
        } else {
            OutputRawAudio(data);
        }
    }
}

void LiteAudioEngine::EnableWakeWordDetection(bool enable) {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    if (!wake_word_) {
        wake_word_enabled_ = false;
        return;
    }

    wake_word_enabled_ = enable;
    if (enable) {
        wake_word_->Start();
    } else {
        wake_word_->Stop();
    }
}

void LiteAudioEngine::EnableVoiceProcessing(bool enable) {
    voice_processing_enabled_ = enable;
    if (!enable) {
        {
            std::lock_guard<std::mutex> lock(aec_mutex_);
            aec_fill_ = 0;
        }
        std::lock_guard<std::mutex> lock(output_mutex_);
        output_buffer_.clear();
    }
}

void LiteAudioEngine::EnableDeviceAec(bool enable) {
#if CONFIG_USE_DEVICE_AEC
    if (enable) {
        if (codec_ == nullptr || !codec_->input_reference() || codec_->input_channels() != 2) {
            ESP_LOGW(TAG, "Device AEC requires a mic + playback reference stereo input");
            return;
        }
        {
            // Unload WakeNet first so its RAM is available to the AEC.
            std::lock_guard<std::mutex> lock(wake_word_mutex_);
            wake_word_suppressed_ = true;
            wake_word_enabled_ = false;
            if (wake_word_) {
                wake_word_->Stop();
                wake_word_.reset();
                ESP_LOGI(TAG, "Released WakeNet for AEC, free internal %u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            }
        }
        bool created;
        {
            std::lock_guard<std::mutex> lock(aec_mutex_);
            created = CreateAecLocked();
        }
        if (!created) {
            // AEC refused: give the wake word back (recreated on next enable).
            std::lock_guard<std::mutex> lock(wake_word_mutex_);
            wake_word_suppressed_ = false;
        }
    } else {
        {
            std::lock_guard<std::mutex> lock(aec_mutex_);
            DestroyAecLocked();
        }
        // WakeNet is recreated lazily by RestoreWakeWordResources().
        std::lock_guard<std::mutex> lock(wake_word_mutex_);
        wake_word_suppressed_ = false;
    }
#else
    if (enable) {
        ESP_LOGW(TAG, "Device AEC is not enabled in this build");
    }
#endif
}

bool LiteAudioEngine::IsDeviceAecActive() {
    std::lock_guard<std::mutex> lock(aec_mutex_);
    return aec_ != nullptr;
}

bool LiteAudioEngine::CreateAecLocked() {
#if CONFIG_USE_DEVICE_AEC
    if (aec_ != nullptr) {
        return true;
    }
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_before < kAecHeapEstimate + kAecHeapReserve) {
        ESP_LOGE(TAG, "Not enough internal RAM for AEC: free %u, need %u + reserve %u",
                 (unsigned)free_before, (unsigned)kAecHeapEstimate, (unsigned)kAecHeapReserve);
        return false;
    }
    // Filter length 4 is the esp-sr recommended default; SR mode keeps speech
    // undistorted for server-side ASR.
    aec_ = aec_create(16000, 4, 1, AEC_MODE_SR_LOW_COST);
    if (aec_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create AEC");
        return false;
    }
    aec_chunk_samples_ = aec_get_chunksize(aec_);
    const size_t bytes = aec_chunk_samples_ * sizeof(int16_t);
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    aec_mic_ = static_cast<int16_t*>(heap_caps_aligned_alloc(16, bytes, caps));
    aec_ref_ = static_cast<int16_t*>(heap_caps_aligned_alloc(16, bytes, caps));
    aec_out_ = static_cast<int16_t*>(heap_caps_aligned_alloc(16, bytes, caps));
    if (aec_mic_ == nullptr || aec_ref_ == nullptr || aec_out_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate AEC buffers");
        DestroyAecLocked();
        return false;
    }
    aec_fill_ = 0;
    aec_time_us_ = 0;
    aec_frames_ = 0;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < kAecHeapReserve) {
        ESP_LOGE(TAG, "AEC left only %u bytes of internal RAM, releasing it",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        DestroyAecLocked();
        return false;
    }
    ESP_LOGI(TAG, "AEC created (%s), chunk %d samples, used %u bytes, free internal %u",
             aec_get_mode_string(AEC_MODE_SR_LOW_COST), aec_chunk_samples_,
             (unsigned)(free_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
#else
    return false;
#endif
}

void LiteAudioEngine::DestroyAecLocked() {
#if CONFIG_USE_DEVICE_AEC
    if (aec_ != nullptr) {
        aec_destroy(aec_);
        aec_ = nullptr;
        ESP_LOGI(TAG, "AEC released");
    }
#endif
    heap_caps_free(aec_mic_);
    heap_caps_free(aec_ref_);
    heap_caps_free(aec_out_);
    aec_mic_ = nullptr;
    aec_ref_ = nullptr;
    aec_out_ = nullptr;
    aec_fill_ = 0;
}

void LiteAudioEngine::ProcessAecLocked(const std::vector<int16_t>& data) {
#if CONFIG_USE_DEVICE_AEC
    // Input is interleaved [mic, reference] frames from the codec.
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        aec_mic_[aec_fill_] = data[i];
        aec_ref_[aec_fill_] = data[i + 1];
        if (++aec_fill_ < aec_chunk_samples_) {
            continue;
        }
        aec_fill_ = 0;

        int64_t start = esp_timer_get_time();
        aec_process(aec_, aec_mic_, aec_ref_, aec_out_);
        aec_time_us_ += esp_timer_get_time() - start;
        if (++aec_frames_ == 1000) {
            // One frame is 32 ms; report the average cost roughly every 32 s.
            ESP_LOGI(TAG, "AEC avg %lld us per %d-sample frame, free internal %u",
                     aec_time_us_ / aec_frames_, aec_chunk_samples_,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            aec_time_us_ = 0;
            aec_frames_ = 0;
        }
        OutputMono(aec_out_, aec_chunk_samples_);
    }
#endif
}

bool LiteAudioEngine::HasWakeWord() const {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    return wake_word_ != nullptr;
}

bool LiteAudioEngine::IsWakeWordDetectionEnabled() const {
    return wake_word_enabled_;
}

bool LiteAudioEngine::IsVoiceProcessingEnabled() const {
    return voice_processing_enabled_;
}

size_t LiteAudioEngine::GetFeedSize() const {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    if (wake_word_) {
        return wake_word_->GetFeedSize();
    }
    return frame_samples_;
}

void LiteAudioEngine::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = std::move(callback);
}

void LiteAudioEngine::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void LiteAudioEngine::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = std::move(callback);
}

void LiteAudioEngine::EncodeWakeWordData() {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

bool LiteAudioEngine::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    return wake_word_ && wake_word_->GetWakeWordOpus(opus);
}

const std::string& LiteAudioEngine::GetLastDetectedWakeWord() const {
    return wake_word_ ? wake_word_->GetLastDetectedWakeWord() : empty_wake_word_;
}

void LiteAudioEngine::ReleaseWakeWordResources() {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_enabled_ = false;
    if (wake_word_) {
        wake_word_->Stop();
        wake_word_.reset();
        ESP_LOGI(TAG, "Released standalone WakeNet resources");
    }
}

bool LiteAudioEngine::RestoreWakeWordResources() {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    return CreateWakeWordLocked();
}

bool LiteAudioEngine::CreateWakeWordLocked() {
    if (!should_have_wake_word_ || wake_word_suppressed_) {
        return true;
    }
    if (wake_word_) {
        return true;
    }

    auto wake_word = std::make_unique<EspWakeWord>();
    wake_word->OnWakeWordDetected([this](const std::string& detected_wake_word) {
        wake_word_enabled_ = false;
        if (wake_word_detected_callback_) {
            wake_word_detected_callback_(detected_wake_word);
        }
    });
    if (!wake_word->Initialize(codec_, models_list_)) {
        ESP_LOGE(TAG, "Failed to initialize standalone WakeNet");
        return false;
    }
    wake_word_ = std::move(wake_word);
    return true;
}

void LiteAudioEngine::OutputRawAudio(const std::vector<int16_t>& data) {
    if (!output_callback_ || codec_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(output_mutex_);
    const size_t channels = codec_->input_channels();
    if (channels <= 1) {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    } else {
        for (size_t i = 0; i < data.size(); i += channels) {
            output_buffer_.push_back(data[i]);
        }
    }
    FlushOutputLocked();
}

void LiteAudioEngine::OutputMono(const int16_t* data, size_t samples) {
    if (!output_callback_) {
        return;
    }

    std::lock_guard<std::mutex> lock(output_mutex_);
    output_buffer_.insert(output_buffer_.end(), data, data + samples);
    FlushOutputLocked();
}

void LiteAudioEngine::FlushOutputLocked() {
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(
                output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}
