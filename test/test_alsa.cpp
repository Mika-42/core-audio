//
// Created by mika on 9/26/26.
//

#include <gtest/gtest.h>
#include <print>
import mka.audio.alsa;

static const char* fmtToStr(mka::audio::Format format) {
    switch (format) {
        case mka::audio::Format::Int16: return "int16";
        case mka::audio::Format::Int24: return "int24";
        case mka::audio::Format::Int32: return "int32";
        case mka::audio::Format::Float32: return "float32";
        case mka::audio::Format::Float64: return "float64";
    }
    std::unreachable();
}

static void printCaps(const mka::audio::StreamCapabilities& caps) {
    std::println("  channels: {} - {}", caps.minChannels, caps.maxChannels);

    std::print("  sample rates: ");
    for (const auto& sampleRate : caps.sampleRates) {
        std::print("{}, ", sampleRate);
    }
    std::print("\n  formats: ");
    for (const auto& format : caps.formats) {
        std::print("{}, ", fmtToStr(format));
    }
    std::print("\n  buffer sizes: ");
    for (const auto& bufferSize : caps.bufferSizes) {
        std::print("{}, ", bufferSize);
    }
    std::println("");
}

TEST(ALSABackendTest, TestGetEndPoints) {
    const mka::audio::ALSA alsa;

    for (const auto endpoints = alsa.getEndPoints(); const auto& endpoint : endpoints) {
        std::println("id: {}", endpoint.id);
        std::println("name: {}", endpoint.name);

        if (endpoint.input) {
            std::println("input:");
            printCaps(*endpoint.input);
        }
        if (endpoint.output) {
            std::println("output:");
            printCaps(*endpoint.output);
        }

        std::println("-----------------");
    }
}

TEST(ALSABackendTest, TestListEndPointsName) {
    const mka::audio::ALSA alsa;

    for (const auto endpoints = alsa.getEndPoints(); const auto& endpoint : endpoints) {
        std::println("{}", endpoint.name);
    }
}