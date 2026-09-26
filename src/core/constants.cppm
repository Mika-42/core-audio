//
// Created by mika on 9/25/26.
//
module;
#include <array>
#include <cstdint>
export module mka.audio.constants;


export namespace mka::audio {
    using SampleRate = std::uint32_t;
    using BufferSize = std::uint32_t;

    enum class Format : std::uint32_t {
        Int16, Int24, Int32, Float32, Float64
    };

    constexpr std::array<SampleRate, 6> supportedSampleRates {
        44'100,
        48'000,
        88'200,
        96'000,
        176'400,
        192'000
    };

    constexpr std::array<Format, 5> supportedFormats {
        Format::Int16,
        Format::Int24,
        Format::Int32,
        Format::Float32,
        Format::Float64
    };

    constexpr std::array<BufferSize, 7> supportedBufferSizes {
        64, 128, 256, 512, 1024, 2048, 4096
    };
}
