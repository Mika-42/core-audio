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

//--- Test Open - coverage : 5 / 7 (71%)


TEST(ALSABackendTest, TestOpenInvalidID) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "InvalidID",
    };

    auto ret = alsa.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::EndpointUnavailable);

    alsa.close();
}

TEST(ALSABackendTest, TestOpenInvalidFmt) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 1,
        .outputChannels = 1,
        .sampleRate = 44100,
        .format = mka::audio::Format::Float32,
        .bufferSize = 512,
    };

    auto ret = alsa.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::FormatNotSupported);
    alsa.close();
}

TEST(ALSABackendTest, TestOpenInvalidChannelCount) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 89,
        .outputChannels = 0,
        .sampleRate = 44100,
        .format = mka::audio::Format::Int32,
        .bufferSize = 512,
    };

    auto ret = alsa.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::ChannelsNotSupported);
    alsa.close();
}

TEST(ALSABackendTest, TestOpenInvalidSamplerate) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 2,
        .outputChannels = 2,
        .sampleRate = 44190,
        .format = mka::audio::Format::Int32,
        .bufferSize = 512,
    };

    auto ret = alsa.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::SampleRateNotSupported);
    alsa.close();
}

TEST(ALSABackendTest, TestOpenInvalidBuffSize) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 2,
        .outputChannels = 2,
        .sampleRate = 44100,
        .format = mka::audio::Format::Int32,
        .bufferSize = 500,
    };

    auto ret = alsa.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::BufferSizeNotSupported);
    alsa.close();
}

TEST(ALSABackendTest, TestOpenPartialFailureCleansUpCapture) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 2,      // valide, s'ouvre normalement
        .outputChannels = 89,    // invalide, échoue après coup
        .sampleRate = 44100,
        .format = mka::audio::Format::Int32,
        .bufferSize = 512,
    };

    auto ret = alsa.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::ChannelsNotSupported);
    alsa.close();
}

TEST(ALSABackendTest, TestOpenTwiceFailsWithInvalidState) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 2,
        .outputChannels = 2,
        .sampleRate = 44100,
        .format = mka::audio::Format::Int32,
        .bufferSize = 512,
    };

    ASSERT_TRUE(alsa.open(config));

    auto ret = alsa.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
    alsa.close();
}

TEST(ALSABackendTest, TestOpenSucceed) {
    mka::audio::ALSA alsa;
    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 2,
        .outputChannels = 2,
        .sampleRate = 44100,
        .format = mka::audio::Format::Int32,
        .bufferSize = 512,
    };

    auto ret = alsa.open(config);
    alsa.close();
    ASSERT_TRUE(ret);
}