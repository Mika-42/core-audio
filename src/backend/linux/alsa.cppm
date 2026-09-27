//
// Created by mika on 9/24/26.
//
module;
#include <algorithm>
#include <cstdint>
#include <expected>
#include <alsa/asoundlib.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

export module mka.audio.alsa;
export import mka.audio.backend;
import mka.audio.constants;

export namespace mka::audio {
    class ALSA final : public Backend {
    protected:
        [[nodiscard]] std::vector<Endpoint> getEndPoints_() const override {
            std::vector<Endpoint> endpoints;

            int cardIndex = -1;
            while (snd_card_next(&cardIndex) >= 0 && cardIndex >= 0) {
                collectCardEndpoints(cardIndex, endpoints);
            }

            return endpoints;
        }

        [[nodiscard]] Result open_(EndpointConfig const &endpointCfg) override {
            const bool needCapture = endpointCfg.direction != Direction::Output;
            const bool needPlayback = endpointCfg.direction != Direction::Input;

            if (needCapture) {
                if (auto result = openStream(endpointCfg, SND_PCM_STREAM_CAPTURE, endpointCfg.inputChannels, captureHandle_, captureAccess_); !result) {
                    return result;
                }
            }

            if (needPlayback) {
                if (auto result = openStream(endpointCfg, SND_PCM_STREAM_PLAYBACK, endpointCfg.outputChannels, playbackHandle_, playbackAccess_); !result) {
                    if (captureHandle_ != nullptr) {
                        snd_pcm_close(captureHandle_);
                        captureHandle_ = nullptr;
                    }
                    return result;
                }
            }

            config_ = endpointCfg;
            return {};
        }

        [[nodiscard]] Result start_() override {
            return {};
        }

        [[nodiscard]] Result stop_() override {
            return {};
        }

        [[nodiscard]] Result close_() override {
            closeHandles();
            return {};
        }

    private:
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

            for (const auto rate: supportedSampleRates) {
                if (snd_pcm_hw_params_test_rate(pcm, hwParams, rate, 0) == 0) {
                    caps.sampleRates.push_back(rate);
                }
            }

            for (const auto format: supportedFormats) {
                if (snd_pcm_hw_params_test_format(pcm, hwParams, toALSAFormat(format)) == 0) {
                    caps.formats.push_back(format);
                }
            }

            for (const auto bufferSize: supportedBufferSizes) {
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

        static snd_pcm_format_t toALSAFormat(const Format format) {
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
            if (snd_pcm_hw_params_set_period_size_near(pcm, params, &period, nullptr) < 0 || period != cfg.bufferSize) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::BufferSizeNotSupported};
            }

            if (snd_pcm_hw_params(pcm, params) < 0) {
                snd_pcm_close(pcm);
                return std::unexpected{ErrorType::ConfigurationFailed};
            }

            outHandle = pcm;
            outAccess = *access;
            return {};
        }

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
    };
}
