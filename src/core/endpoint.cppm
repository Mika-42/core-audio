//
// Created by mika on 9/25/26.
//
module;
#include <cstdint>
#include <string>
#include <vector>
export module mka.audio.endpoint;
import mka.audio.constants;

export namespace mka::audio {
    enum class Direction { Input, Output, Duplex };

    struct Endpoint {
        std::string id;
        std::string name;

        Direction direction;

        std::uint32_t inputChannelCount;
        std::uint32_t outputChannelCount;

        std::vector<SampleRate> sampleRates;
        std::vector<Format> formats;
        std::vector<BufferSize> bufferSizes;
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
