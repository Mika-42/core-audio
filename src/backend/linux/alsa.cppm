//
// Created by mika on 9/24/26.
//
module;
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <alsa/asoundlib.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

export module mka.audio.alsa;
export import mka.audio.backend;
import mka.audio.constants;
import mka.audio.process;

export namespace mka::audio {
    class ALSA final : public Backend {
    public:
        ~ALSA() override {
            if (audioThread_.joinable()) {
                audioThread_.request_stop();
                audioThread_.join();
            }
            closeHandles();
        }

    protected:
        [[nodiscard]] std::vector<Endpoint> getEndPoints_() const override {
            std::vector<Endpoint> endpoints;

            try {
                int cardIndex = -1;
                while (snd_card_next(&cardIndex) >= 0 && cardIndex >= 0) {
                    collectCardEndpoints(cardIndex, endpoints);
                }
            } catch (...) {
                return {};
            }

            return endpoints;
        }

        [[nodiscard]] Result open_(EndpointConfig const &endpointCfg) override {
            const bool needCapture = endpointCfg.direction != Direction::Output;
            const bool needPlayback = endpointCfg.direction != Direction::Input;

            if (needCapture) {
                if (auto result = openStream(endpointCfg, SND_PCM_STREAM_CAPTURE,
                                              endpointCfg.inputChannels, captureHandle_, captureAccess_); !result) {
                    return result;
                }
            }

            if (needPlayback) {
                if (auto result = openStream(endpointCfg, SND_PCM_STREAM_PLAYBACK,
                                              endpointCfg.outputChannels, playbackHandle_, playbackAccess_); !result) {
                    if (captureHandle_ != nullptr) {
                        snd_pcm_close(captureHandle_);
                        captureHandle_ = nullptr;
                    }
                    return result;
                }
            }

            config_ = endpointCfg;
            allocateScratchBuffers(endpointCfg, needCapture, needPlayback);

            return {};
        }

        [[nodiscard]] Result start_() override {
            try {
                threadReady_ = false;

                audioThread_ = std::jthread([this](const std::stop_token &stopToken) {
                    {
                        std::lock_guard lock(threadMutex_);
                        threadReady_ = true;
                    }
                    threadReadyCv_.notify_one();

                    audioLoop(stopToken);
                });

                std::unique_lock lock(threadMutex_);
                threadReadyCv_.wait(lock, [this] { return threadReady_; });
            } catch (...) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            return {};
        }

        [[nodiscard]] Result stop_() override {
            if (!audioThread_.joinable()) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            try {
                audioThread_.request_stop();
                audioThread_.join();
            } catch (...) {
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            // Remet les flux en état PREPARED : sans ça, un prochain start_() appellerait
            // snd_pcm_start sur un device resté RUNNING/DRAINING, ce qui échouerait.
            if (captureHandle_ != nullptr) {
                snd_pcm_drop(captureHandle_);
                snd_pcm_prepare(captureHandle_);
            }
            if (playbackHandle_ != nullptr) {
                snd_pcm_drop(playbackHandle_);
                snd_pcm_prepare(playbackHandle_);
            }

            return {};
        }

        [[nodiscard]] Result close_() override {
            closeHandles();
            return {};
        }

    private:
        // --- getEndPoints_ -----------------------------------------------------

        static void collectCardEndpoints(const int cardIndex, std::vector<Endpoint> &endpoints) {
            const std::string ctlName = "hw:" + std::to_string(cardIndex);

            snd_ctl_t *ctl = nullptr;
            if (snd_ctl_open(&ctl, ctlName.c_str(), 0) < 0) {
                return;
            }

            snd_ctl_card_info_t *cardInfo = nullptr;
            snd_ctl_card_info_alloca(&cardInfo);

            std::string cardName = ctlName;
            if (snd_ctl_card_info(ctl, cardInfo) >= 0) {
                cardName = snd_ctl_card_info_get_name(cardInfo);
            }

            int deviceIndex = -1;
            while (snd_ctl_pcm_next_device(ctl, &deviceIndex) >= 0 && deviceIndex >= 0) {
                if (auto endpoint = buildDeviceEndpoint(ctl, cardIndex, deviceIndex, cardName)) {
                    endpoints.push_back(std::move(*endpoint));
                }
            }

            snd_ctl_close(ctl);
        }

        static std::optional<Endpoint> buildDeviceEndpoint(snd_ctl_t *ctl, const int cardIndex,
                                                             const int deviceIndex,
                                                             const std::string &cardName) {
            const bool hasPlayback = pcmStreamExists(ctl, deviceIndex, SND_PCM_STREAM_PLAYBACK);
            const bool hasCapture = pcmStreamExists(ctl, deviceIndex, SND_PCM_STREAM_CAPTURE);

            if (!hasPlayback && !hasCapture) {
                return std::nullopt;
            }

            const std::string id = "hw:" + std::to_string(cardIndex) + "," + std::to_string(deviceIndex);
            const std::string pcmName = getPcmDeviceName(
                ctl, deviceIndex, hasPlayback ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE);

            Endpoint endpoint{};
            endpoint.id = id;
            endpoint.name = pcmName.empty() ? cardName : cardName + " - " + pcmName;

            if (hasCapture) {
                endpoint.input = queryStreamCaps(id, SND_PCM_STREAM_CAPTURE);
            }
            if (hasPlayback) {
                endpoint.output = queryStreamCaps(id, SND_PCM_STREAM_PLAYBACK);
            }

            if (!endpoint.input && !endpoint.output) {
                return std::nullopt;
            }

            return endpoint;
        }

        static bool pcmStreamExists(snd_ctl_t *ctl, const int deviceIndex, const snd_pcm_stream_t stream) {
            snd_pcm_info_t *info = nullptr;
            snd_pcm_info_alloca(&info);
            snd_pcm_info_set_device(info, deviceIndex);
            snd_pcm_info_set_subdevice(info, 0);
            snd_pcm_info_set_stream(info, stream);
            return snd_ctl_pcm_info(ctl, info) >= 0;
        }

        static std::string getPcmDeviceName(snd_ctl_t *ctl, const int deviceIndex,
                                             const snd_pcm_stream_t stream) {
            snd_pcm_info_t *info = nullptr;
            snd_pcm_info_alloca(&info);
            snd_pcm_info_set_device(info, deviceIndex);
            snd_pcm_info_set_subdevice(info, 0);
            snd_pcm_info_set_stream(info, stream);

            if (snd_ctl_pcm_info(ctl, info) < 0) {
                return {};
            }

            return snd_pcm_info_get_name(info);
        }

        static std::optional<StreamCapabilities> queryStreamCaps(const std::string &id,
                                                                   const snd_pcm_stream_t stream) {
            snd_pcm_t *pcm = nullptr;
            if (snd_pcm_open(&pcm, id.c_str(), stream, SND_PCM_NONBLOCK) < 0) {
                return std::nullopt;
            }

            snd_pcm_hw_params_t *hwParams = nullptr;
            snd_pcm_hw_params_alloca(&hwParams);

            if (snd_pcm_hw_params_any(pcm, hwParams) < 0) {
                snd_pcm_close(pcm);
                return std::nullopt;
            }

            StreamCapabilities caps{};

            unsigned int minChannels = 0;
            unsigned int maxChannels = 0;
            snd_pcm_hw_params_get_channels_min(hwParams, &minChannels);
            snd_pcm_hw_params_get_channels_max(hwParams, &maxChannels);
            caps.minChannels = minChannels;
            caps.maxChannels = maxChannels;

            for (const auto rate : supportedSampleRates) {
                if (snd_pcm_hw_params_test_rate(pcm, hwParams, rate, 0) == 0) {
                    caps.sampleRates.push_back(rate);
                }
            }

            for (const auto format : supportedFormats) {
                if (snd_pcm_hw_params_test_format(pcm, hwParams, toALSAFormat(format)) == 0) {
                    caps.formats.push_back(format);
                }
            }

            for (const auto bufferSize : supportedBufferSizes) {
                if (const snd_pcm_uframes_t frames = bufferSize; snd_pcm_hw_params_test_period_size(
                                                                      pcm, hwParams, frames, 0) == 0) {
                    caps.bufferSizes.push_back(bufferSize);
                }
            }

            snd_pcm_close(pcm);

            if (caps.sampleRates.empty() && caps.formats.empty() && caps.bufferSizes.empty()) {
                return std::nullopt;
            }

            return caps;
        }

        // --- open_ ---------------------------------------------------------------

        static std::optional<snd_pcm_access_t> negotiateAccess(snd_pcm_t *pcm,
                                                                 snd_pcm_hw_params_t *params) noexcept {
            if (snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_NONINTERLEAVED) == 0) {
                return SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
            }

            if (snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_MMAP_INTERLEAVED) == 0) {
                return SND_PCM_ACCESS_MMAP_INTERLEAVED;
            }

            return std::nullopt;
        }

        static Result openStream(const EndpointConfig &cfg, const snd_pcm_stream_t stream,
                                  const std::uint32_t channels, snd_pcm_t *&outHandle,
                                  snd_pcm_access_t &outAccess) noexcept {
            snd_pcm_t *pcm = nullptr;
            if (snd_pcm_open(&pcm, cfg.id.c_str(), stream, 0) < 0) {
                return std::unexpected{ErrorType::EndpointUnavailable};
            }

            snd_pcm_hw_params_t *params = nullptr;
            snd_pcm_hw_params_alloca(&params);

            if (snd_pcm_hw_params_any(pcm, params) < 0) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            const auto access = negotiateAccess(pcm, params);
            if (!access) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            if (snd_pcm_hw_params_set_format(pcm, params, toALSAFormat(cfg.format)) < 0) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::FormatNotSupported};
            }

            if (snd_pcm_hw_params_set_channels(pcm, params, channels) < 0) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::ChannelsNotSupported};
            }

            unsigned int rate = cfg.sampleRate;
            if (snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr) < 0 || rate != cfg.sampleRate) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::SampleRateNotSupported};
            }

            snd_pcm_uframes_t period = cfg.bufferSize;
            if (snd_pcm_hw_params_set_period_size_near(pcm, params, &period, nullptr) < 0
                || period != cfg.bufferSize) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::BufferSizeNotSupported};
            }

            if (snd_pcm_hw_params(pcm, params) < 0) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            if (const int err = snd_pcm_prepare(pcm); err < 0) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            outHandle = pcm;
            outAccess = *access;
            return {};
        }

        void allocateScratchBuffers(const EndpointConfig &cfg, const bool needCapture,
                                     const bool needPlayback) {
            if (needCapture) {
                inputScratch_.assign(cfg.inputChannels, std::vector<float>(cfg.bufferSize));
                inputChannelPtrs_.resize(cfg.inputChannels);
                for (std::uint32_t i = 0; i < cfg.inputChannels; ++i) {
                    inputChannelPtrs_[i] = inputScratch_[i].data();
                }
            }

            if (needPlayback) {
                outputScratch_.assign(cfg.outputChannels, std::vector<float>(cfg.bufferSize));
                outputChannelPtrs_.resize(cfg.outputChannels);
                for (std::uint32_t i = 0; i < cfg.outputChannels; ++i) {
                    outputChannelPtrs_[i] = outputScratch_[i].data();
                }
            }
        }

        static snd_pcm_format_t toALSAFormat(const Format format) noexcept {
            switch (format) {
                case Format::Int16:
                    return SND_PCM_FORMAT_S16;

                case Format::Int24:
                    return SND_PCM_FORMAT_S24;

                case Format::Int32:
                    return SND_PCM_FORMAT_S32;

                case Format::Float32:
                    return SND_PCM_FORMAT_FLOAT;

                case Format::Float64:
                    return SND_PCM_FORMAT_FLOAT64;
            }

            std::unreachable();
        }

        // --- start_ ----------------------------------------------------------------

        static float sampleToFloat(const std::byte *ptr, const Format format) noexcept {
            switch (format) {
                case Format::Int16:
                    return static_cast<float>(*reinterpret_cast<const std::int16_t *>(ptr)) / 32768.0f;

                case Format::Int24:
                case Format::Int32:
                    return static_cast<float>(*reinterpret_cast<const std::int32_t *>(ptr)) / 2147483648.0f;

                case Format::Float32:
                    return *reinterpret_cast<const float *>(ptr);

                case Format::Float64:
                    return static_cast<float>(*reinterpret_cast<const double *>(ptr));
            }
            std::unreachable();
        }

        static void floatToSample(std::byte *ptr, const Format format, const float value) noexcept {
            switch (format) {
                case Format::Int16:
                    *reinterpret_cast<std::int16_t *>(ptr) = static_cast<std::int16_t>(value * 32767.0f);
                    return;

                case Format::Int24:
                case Format::Int32:
                    *reinterpret_cast<std::int32_t *>(ptr) = static_cast<std::int32_t>(value * 2147483647.0f);
                    return;

                case Format::Float32:
                    *reinterpret_cast<float *>(ptr) = value;
                    return;

                case Format::Float64:
                    *reinterpret_cast<double *>(ptr) = static_cast<double>(value);
                    return;
            }
        }

        static snd_pcm_uframes_t mmapReadToScratch(snd_pcm_t *pcm, const Format format,
                                                     std::vector<std::vector<float> > &scratch) noexcept {
            const snd_pcm_channel_area_t *areas = nullptr;
            snd_pcm_uframes_t offset = 0;
            snd_pcm_uframes_t frames = scratch.empty() ? 0 : scratch[0].size();

            if (frames == 0) {
                return 0;
            }

            if (const int err = snd_pcm_mmap_begin(pcm, &areas, &offset, &frames); err < 0) {
                return 0;
            }

            for (std::size_t ch = 0; ch < scratch.size(); ++ch) {
                auto *base = static_cast<std::byte *>(areas[ch].addr) + areas[ch].first / 8;
                const std::size_t strideBytes = areas[ch].step / 8;

                for (snd_pcm_uframes_t i = 0; i < frames; ++i) {
                    scratch[ch][i] = sampleToFloat(base + (offset + i) * strideBytes, format);
                }
            }

            snd_pcm_mmap_commit(pcm, offset, frames);
            return frames;
        }

        static void mmapWriteFromScratch(snd_pcm_t *pcm, const Format format,
                                          const std::vector<std::vector<float> > &scratch,
                                          const snd_pcm_uframes_t frames) noexcept {
            const snd_pcm_channel_area_t *areas = nullptr;
            snd_pcm_uframes_t offset = 0;
            snd_pcm_uframes_t avail = frames;

            if (frames == 0 || snd_pcm_mmap_begin(pcm, &areas, &offset, &avail) < 0) {
                return;
            }

            const snd_pcm_uframes_t toWrite = std::min(avail, frames);

            for (std::size_t ch = 0; ch < scratch.size(); ++ch) {
                auto *base = static_cast<std::byte *>(areas[ch].addr) + areas[ch].first / 8;
                const std::size_t strideBytes = areas[ch].step / 8;

                for (snd_pcm_uframes_t i = 0; i < toWrite; ++i) {
                    floatToSample(base + (offset + i) * strideBytes, format, scratch[ch][i]);
                }
            }

            snd_pcm_mmap_commit(pcm, offset, toWrite);
        }

        static void recoverStream(snd_pcm_t *pcm) noexcept {
            if (snd_pcm_prepare(pcm) >= 0) {
                snd_pcm_start(pcm);
            }
        }

        void audioLoop(const std::stop_token &stopToken) noexcept {
            snd_pcm_t *waitHandle = captureHandle_ != nullptr ? captureHandle_ : playbackHandle_;
            if (waitHandle == nullptr) {
                return;
            }

            if (captureHandle_ != nullptr) {
                const int err = snd_pcm_start(captureHandle_);
            }

            bool playbackStarted = playbackHandle_ == nullptr;

            int iterations = 0;
            while (!stopToken.stop_requested()) {
                const int waitResult = snd_pcm_wait(waitHandle, 100);
                if (iterations < 10) {
                    ++iterations;
                }

                if (waitResult <= 0) {
                    continue;
                }

                AudioProcessContext ctx{};

                if (captureHandle_ != nullptr) {
                    ctx.frames = mmapReadToScratch(captureHandle_, config_.format, inputScratch_);
                    if (ctx.frames == 0) {
                        recoverStream(captureHandle_);
                        continue;
                    }
                    ctx.input.channels = inputChannelPtrs_.data();
                    ctx.input.count = static_cast<std::uint32_t>(inputChannelPtrs_.size());
                } else {
                    ctx.frames = config_.bufferSize;
                }

                if (playbackHandle_ != nullptr) {
                    ctx.output.channels = outputChannelPtrs_.data();
                    ctx.output.count = static_cast<std::uint32_t>(outputChannelPtrs_.size());
                }

                if (callback != nullptr) {
                    callback(userData, ctx);
                }

                if (playbackHandle_ != nullptr) {
                    mmapWriteFromScratch(playbackHandle_, config_.format, outputScratch_, ctx.frames);

                    if (!playbackStarted) {
                        snd_pcm_start(playbackHandle_);
                        playbackStarted = true;
                    }
                }
            }
        }

        // --- close_ / destructor -----------------------------------------------

        void closeHandles() noexcept {
            if (captureHandle_ != nullptr) {
                snd_pcm_close(captureHandle_);
                captureHandle_ = nullptr;
            }
            if (playbackHandle_ != nullptr) {
                snd_pcm_close(playbackHandle_);
                playbackHandle_ = nullptr;
            }
        }

        snd_pcm_t *captureHandle_ = nullptr;
        snd_pcm_t *playbackHandle_ = nullptr;
        snd_pcm_access_t captureAccess_{};
        snd_pcm_access_t playbackAccess_{};
        EndpointConfig config_{};

        std::vector<std::vector<float> > inputScratch_;
        std::vector<std::vector<float> > outputScratch_;
        std::vector<float *> inputChannelPtrs_;
        std::vector<float *> outputChannelPtrs_;

        std::jthread audioThread_;
        std::mutex threadMutex_;
        std::condition_variable threadReadyCv_;
        bool threadReady_ = false;
    };
}
