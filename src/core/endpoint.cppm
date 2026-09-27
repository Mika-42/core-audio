//
// Created by mika on 9/25/26.
//
module;
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
export module mka.audio.endpoint;
import mka.audio.constants;

export namespace mka::audio {
    enum class Direction { Input, Output, Duplex };

    struct StreamCapabilities {
        std::uint32_t minChannels;
        std::uint32_t maxChannels;

        std::vector<SampleRate> sampleRates;
        std::vector<Format> formats;
        std::vector<BufferSize> bufferSizes;
    };

    struct Endpoint {
        std::string id;
        std::string name;

        std::optional<StreamCapabilities> input;
        std::optional<StreamCapabilities> output;
    };

    struct EndpointConfig {
        std::string id;
        Direction direction;

        std::uint32_t inputChannels;
        std::uint32_t outputChannels;

        SampleRate sampleRate;
        Format format;
        BufferSize bufferSize;
    };
}
