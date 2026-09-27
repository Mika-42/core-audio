//
// Created by mika on 9/24/26.
//
module;
#include <algorithm>
#include <cstdint>
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
            return {};
        }

        [[nodiscard]] Result start_() override {
            return {};
        }

        [[nodiscard]] Result stop_() override {
            return {};
        }

        [[nodiscard]] Result close_() override {
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
    };
}