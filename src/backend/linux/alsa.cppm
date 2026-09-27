//
// Created by mika on 9/24/26.
//
module;
#include <algorithm>
#include <cstdint>
#include <alsa/asoundlib.h>
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

            void **hints = nullptr;
            if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
                return {};
            }

            for (void **hint = hints; *hint != nullptr; ++hint) {
                const std::string ioid = getHintProperty(*hint, HintProperty::IOID);
                const std::string name = getHintProperty(*hint, HintProperty::Name);
                const std::string desc = getHintProperty(*hint, HintProperty::Description);

                // skip invalid names
                if (name.empty() || name == "null") {
                    continue;
                }

                // get direction
                const bool queryInput = ioid.empty() || ioid == "Input";
                const bool queryOutput = ioid.empty() || ioid == "Output";

                Endpoint endpoint{};
                endpoint.id = name;
                endpoint.name = cleanName(desc.empty() ? name : desc);
                endpoint.inputChannelCount = 0;
                endpoint.outputChannelCount = 0;

                std::vector<SampleRate> rates;
                std::vector<Format> formats;
                std::vector<BufferSize> bufferSizes;

                if (queryInput) {
                    queryStreamCaps(name, SND_PCM_STREAM_CAPTURE,
                                    endpoint.inputChannelCount, rates, formats, bufferSizes);
                }
                if (queryOutput) {
                    queryStreamCaps(name, SND_PCM_STREAM_PLAYBACK,
                                    endpoint.outputChannelCount, rates, formats, bufferSizes);
                }

                if (queryInput && queryOutput) {
                    endpoint.direction = Direction::Duplex;
                } else if (queryInput) {
                    endpoint.direction = Direction::Input;
                } else {
                    endpoint.direction = Direction::Output;
                }

                std::ranges::sort(rates);
                rates.erase(std::ranges::unique(rates).begin(), rates.end());

                std::ranges::sort(formats);
                formats.erase(std::ranges::unique(formats).begin(), formats.end());

                std::ranges::sort(bufferSizes);
                bufferSizes.erase(std::ranges::unique(bufferSizes).begin(), bufferSizes.end());

                endpoint.sampleRates = std::move(rates);
                endpoint.formats = std::move(formats);
                endpoint.bufferSizes = std::move(bufferSizes);

                endpoints.push_back(std::move(endpoint));
            }
            snd_device_name_free_hint(hints);

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

        enum class HintProperty {
            Name, Description, IOID
        };

        static const char *hintToStr(const HintProperty property) {
            switch (property) {
                case HintProperty::Name: return "NAME";
                case HintProperty::Description: return "DESC";
                case HintProperty::IOID: return "IOID";
            }
            std::unreachable();
        }

        static std::string getHintProperty(const void *hint, const HintProperty prop) {
            char *p = snd_device_name_get_hint(hint, hintToStr(prop));

            if (p == nullptr) {
                return {};
            }

            std::string result{p};

            free(p);

            return result;
        }

        static void queryStreamCaps(const std::string &id, const snd_pcm_stream_t stream,
                                    std::uint32_t &channelCount,
                                    std::vector<SampleRate> &rates,
                                    std::vector<Format> &formats,
                                    std::vector<BufferSize> &bufferSizes) {
            snd_pcm_t *pcm = nullptr;
            if (snd_pcm_open(&pcm, id.c_str(), stream, SND_PCM_NONBLOCK) < 0) {
                return;
            }

            snd_pcm_hw_params_t *hwParams = nullptr;
            snd_pcm_hw_params_alloca(&hwParams);

            if (snd_pcm_hw_params_any(pcm, hwParams) < 0) {
                snd_pcm_close(pcm);
                return;
            }

            unsigned int maxChannels = 0;
            if (snd_pcm_hw_params_get_channels_max(hwParams, &maxChannels) >= 0) {
                channelCount = maxChannels;
            }

            for (const auto rate: supportedSampleRates) {
                if (snd_pcm_hw_params_test_rate(pcm, hwParams, rate, 0) == 0) {
                    rates.push_back(rate);
                }
            }

            for (const auto format: supportedFormats) {
                if (snd_pcm_hw_params_test_format(pcm, hwParams, toALSAFormat(format)) == 0) {
                    formats.push_back(format);
                }
            }

            for (const auto bufferSize: supportedBufferSizes) {
                if (const snd_pcm_uframes_t frames = bufferSize; snd_pcm_hw_params_test_period_size(
                                                                     pcm, hwParams, frames, 0) == 0) {
                    bufferSizes.push_back(bufferSize);
                }
            }

            snd_pcm_close(pcm);
        }

        static std::string cleanName(std::string name) {
            name.erase(
                std::ranges::remove_if(name, [](const unsigned char c) {
                    return c == '\n' || c == '\r' || c == '\t';
                }).begin(),
                name.end()
            );

            return name;
        }
    };
}
