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

TEST(ALSABackendTest, TestGetEndPoints) {
    const mka::audio::ALSA alsa;
    const auto endpoints = alsa.getEndPoints();

    for (const auto& endpoint : endpoints) {
        std::println("id: {}", endpoint.id);
        std::println("name: {}", endpoint.name);
        std::println("direction: {}", endpoint.direction == mka::audio::Direction::Input ? "input" : endpoint.direction == mka::audio::Direction::Output ? "output" : "duplex");
        std::println("input channel count: {}", endpoint.inputChannelCount);
        std::println("output channel count: {}", endpoint.outputChannelCount);
        std::print("input sample rate: ");
        for (const auto& sampleRate : endpoint.sampleRates) {
            std::print("{}, ", sampleRate);
        }
        std::print("\nformats: ");
        for (const auto& format : endpoint.formats) {
            std::print("{}, ", fmtToStr(format));
        }
        std::print("\nbuffer size: ");
        for (const auto& bufferSize : endpoint.bufferSizes) {
            std::print("{}, ", bufferSize);
        }
        std::println("\n-----------------");
    }
}
