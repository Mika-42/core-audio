//
// Created by mika on 9/26/26.
//

#include <gtest/gtest.h>
#include <print>
#include <cmath>

import mka.audio.alsa;
import mka.audio.process;

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
    const auto endpoints = alsa.getEndPoints();

    for (const auto& endpoint : endpoints) {

        EXPECT_TRUE(endpoint.input.has_value() || endpoint.output.has_value())
            << "endpoint " << endpoint.id << " has neither input nor output";

        EXPECT_FALSE(endpoint.id.empty());
        EXPECT_FALSE(endpoint.name.empty());

        if (endpoint.input) {
            EXPECT_LE(endpoint.input->minChannels, endpoint.input->maxChannels);
            EXPECT_FALSE(endpoint.input->sampleRates.empty()
                         && endpoint.input->formats.empty()
                         && endpoint.input->bufferSizes.empty())
                << "endpoint " << endpoint.id << " has empty input capabilities";
        }
        if (endpoint.output) {
            EXPECT_LE(endpoint.output->minChannels, endpoint.output->maxChannels);
            EXPECT_FALSE(endpoint.output->sampleRates.empty()
                         && endpoint.output->formats.empty()
                         && endpoint.output->bufferSizes.empty())
                << "endpoint " << endpoint.id << " has empty output capabilities";
        }

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

TEST(ALSABackendTest, TestGetEndPointsContainsKnownDevice) {
    const mka::audio::ALSA alsa;
    const auto endpoints = alsa.getEndPoints();

    const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.id == "hw:1,0";
    });

    ASSERT_NE(it, endpoints.end());
    EXPECT_TRUE(it->input.has_value());
    EXPECT_TRUE(it->output.has_value());
}

//--- Test Open

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
        .inputChannels = 2,
        .outputChannels = 89,
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

namespace {
    std::atomic<bool> g_callbackCalled{false};
    std::atomic<std::thread::id> g_callbackThreadId{};

    void testCallback(const mka::audio::AudioProcessContext&) noexcept {
        g_callbackThreadId.store(std::this_thread::get_id());
        g_callbackCalled.store(true);
    }
}

//--- Test Start

TEST(ALSABackendTest, TestStartWithoutOpenFailsInvalidState) {
    mka::audio::ALSA alsa;

    auto ret = alsa.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(ALSABackendTest, TestStartSucceedsAfterOpen) {
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

    auto ret = alsa.start();
    ASSERT_TRUE(ret);
}

TEST(ALSABackendTest, TestStartTwiceFailsInvalidState) {
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
    ASSERT_TRUE(alsa.start());

    auto ret = alsa.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(ALSABackendTest, TestStartInvokesCallbackOnDifferentThread) {
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

    g_callbackCalled.store(false);
    g_callbackThreadId.store({});

    ASSERT_TRUE(alsa.setProcessFunction(testCallback));
    ASSERT_TRUE(alsa.open(config));

    const auto callingThreadId = std::this_thread::get_id();
    ASSERT_TRUE(alsa.start());

    for (int i = 0; i < 100 && !g_callbackCalled.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ASSERT_TRUE(g_callbackCalled.load());
    ASSERT_NE(g_callbackThreadId.load(), callingThreadId);
}

TEST(ALSABackendTest, TestSetProcessFunctionFailsWhileRunning) {
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
    ASSERT_TRUE(alsa.start());

    auto ret = alsa.setProcessFunction(testCallback);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

//--- Test Stop

TEST(ALSABackendTest, TestStopWithoutStartFailsInvalidState) {
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

    auto ret = alsa.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(ALSABackendTest, TestStopSucceedsAfterStart) {
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
    ASSERT_TRUE(alsa.start());

    auto ret = alsa.stop();
    ASSERT_TRUE(ret);
}

TEST(ALSABackendTest, TestStopTwiceFailsInvalidState) {
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
    ASSERT_TRUE(alsa.start());
    ASSERT_TRUE(alsa.stop());

    auto ret = alsa.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(ALSABackendTest, TestStopActuallyHaltsCallbackInvocations) {
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

    g_callbackCalled.store(false);

    ASSERT_TRUE(alsa.setProcessFunction(testCallback));
    ASSERT_TRUE(alsa.open(config));
    ASSERT_TRUE(alsa.start());

    for (int i = 0; i < 100 && !g_callbackCalled.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(g_callbackCalled.load());

    ASSERT_TRUE(alsa.stop());

    g_callbackCalled.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_FALSE(g_callbackCalled.load());
}

TEST(ALSABackendTest, TestStopAllowsReopenAndRestart) {
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
    ASSERT_TRUE(alsa.start());
    ASSERT_TRUE(alsa.stop());

    auto ret = alsa.start();
    ASSERT_TRUE(ret);
    ASSERT_TRUE(alsa.stop());
}

//--- Test Close

TEST(ALSABackendTest, TestCloseWithoutOpenFailsInvalidState) {
    mka::audio::ALSA alsa;

    auto ret = alsa.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(ALSABackendTest, TestCloseSucceedsAfterOpen) {
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

    auto ret = alsa.close();
    ASSERT_TRUE(ret);
}

TEST(ALSABackendTest, TestCloseFailsWhileRunning) {
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
    ASSERT_TRUE(alsa.start());

    // Backend impose l'ordre open -> start -> stop -> close : close_ doit être
    // refusé tant que le flux tourne encore (state == Running, pas Open).
    auto ret = alsa.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(alsa.stop());
    ASSERT_TRUE(alsa.close());
}

TEST(ALSABackendTest, TestCloseTwiceFailsInvalidState) {
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
    ASSERT_TRUE(alsa.close());

    auto ret = alsa.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(ALSABackendTest, TestCloseReleasesDeviceForReopen) {
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
    ASSERT_TRUE(alsa.close());

    // Si close_ ne libère pas vraiment les handles ALSA (snd_pcm_close), le
    // device resterait marqué occupé et ce second open échouerait en
    // EndpointUnavailable plutôt que de réussir.
    auto ret = alsa.open(config);
    ASSERT_TRUE(ret);
    alsa.close();
}

TEST(ALSABackendTest, TestFullLifecycleOpenStartStopClose) {
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
    ASSERT_TRUE(alsa.start());
    ASSERT_TRUE(alsa.stop());
    ASSERT_TRUE(alsa.close());
}
//--- Test Sinewave
namespace {
    double g_sinePhase = 0.0;
    double g_sinePhaseIncrement = 0.0;

    void sineCallback(const mka::audio::AudioProcessContext &ctx) noexcept {
        for (std::uint32_t i = 0; i < ctx.frames; ++i) {
            constexpr float amplitude = 0.2f;
            const auto sample = static_cast<float>(std::sin(g_sinePhase) * amplitude);

            for (std::uint32_t ch = 0; ch < ctx.output.count; ++ch) {
                ctx.output.channels[ch][i] = sample;
            }

            g_sinePhase += g_sinePhaseIncrement;
            if (g_sinePhase >= 2.0 * std::numbers::pi) {
                g_sinePhase -= 2.0 * std::numbers::pi;
            }
        }
    }
}

// To togle remove DISABLED_
TEST(ALSABackendTest, DISABLED_TestPlaySineWave) {
    mka::audio::ALSA alsa;

    constexpr mka::audio::SampleRate sampleRate = 44100;
    constexpr double frequency = 440.0;

    const mka::audio::EndpointConfig config {
        .id = "hw:1,0",
        .direction = mka::audio::Direction::Output,
        .inputChannels = 0,
        .outputChannels = 2,
        .sampleRate = sampleRate,
        .format = mka::audio::Format::Int32,
        .bufferSize = 512,
    };

    g_sinePhase = 0.0;
    g_sinePhaseIncrement = 2.0 * std::numbers::pi * frequency / static_cast<double>(sampleRate);

    ASSERT_TRUE(alsa.setProcessFunction(sineCallback));
    ASSERT_TRUE(alsa.open(config));
    ASSERT_TRUE(alsa.start());

    std::this_thread::sleep_for(std::chrono::seconds(3));

    ASSERT_TRUE(alsa.stop());
    ASSERT_TRUE(alsa.close());
}

namespace {
    std::vector<std::vector<float>> g_recordedSamples;
    std::atomic<std::size_t> g_recordedFrames{0};
    std::size_t g_recordCapacityFrames = 0;

    void recordCallback(const mka::audio::AudioProcessContext &ctx) noexcept {
        const std::size_t framesLeft = g_recordCapacityFrames - g_recordedFrames.load();
        const std::size_t framesToCopy = std::min<std::size_t>(framesLeft, ctx.frames);

        const std::size_t writeOffset = g_recordedFrames.load();
        for (std::uint32_t ch = 0; ch < ctx.input.count && ch < g_recordedSamples.size(); ++ch) {
            for (std::size_t i = 0; i < framesToCopy; ++i) {
                g_recordedSamples[ch][writeOffset + i] = ctx.input.channels[ch][i];
            }
        }

        g_recordedFrames.fetch_add(framesToCopy);
    }

    std::atomic<std::size_t> g_playbackFrames{0};

    void playbackCallback(const mka::audio::AudioProcessContext &ctx) noexcept {
        const std::size_t framesLeft = g_recordCapacityFrames - g_playbackFrames.load();
        const std::size_t framesToCopy = std::min<std::size_t>(framesLeft, ctx.frames);

        const std::size_t readOffset = g_playbackFrames.load();
        for (std::uint32_t ch = 0; ch < ctx.output.count; ++ch) {
            const std::uint32_t srcCh = ch < g_recordedSamples.size() ? ch : 0;
            for (std::size_t i = 0; i < framesToCopy; ++i) {
                ctx.output.channels[ch][i] = g_recordedSamples[srcCh][readOffset + i];
            }
            for (std::size_t i = framesToCopy; i < ctx.frames; ++i) {
                ctx.output.channels[ch][i] = 0.0f; // silence une fois l'enregistrement épuisé
            }
        }

        g_playbackFrames.fetch_add(framesToCopy);
    }
}

/*
 * record 5s
 * Wait 3s
 * play recording
 */
TEST(ALSABackendTest, DISABLED_TestRecordAndPlayback) {
    constexpr mka::audio::SampleRate sampleRate = 48000;
    constexpr std::uint32_t channels = 2;
    constexpr int recordSeconds = 5;

    g_recordCapacityFrames = static_cast<std::size_t>(sampleRate) * recordSeconds;
    g_recordedSamples.assign(channels, std::vector<float>(g_recordCapacityFrames, 0.0f));
    g_recordedFrames.store(0);
    g_playbackFrames.store(0);

    // --- Phase 1 : recording ---
    {
        mka::audio::ALSA recorder;
        const mka::audio::EndpointConfig recordConfig {
            .id = "hw:2,0",
            .direction = mka::audio::Direction::Input,
            .inputChannels = channels,
            .outputChannels = 0,
            .sampleRate = sampleRate,
            .format = mka::audio::Format::Int32,
            .bufferSize = 512,
        };

        ASSERT_TRUE(recorder.setProcessFunction(recordCallback));
        ASSERT_TRUE(recorder.open(recordConfig));
        ASSERT_TRUE(recorder.start());

        while (g_recordedFrames.load() < g_recordCapacityFrames) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        ASSERT_TRUE(recorder.stop());
        ASSERT_TRUE(recorder.close());
    }

    // --- Wait ---
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // --- Phase 2 : playback ---
    {
        mka::audio::ALSA player;
        const mka::audio::EndpointConfig playbackConfig {
            .id = "hw:1,0",
            .direction = mka::audio::Direction::Output,
            .inputChannels = 0,
            .outputChannels = channels,
            .sampleRate = sampleRate,
            .format = mka::audio::Format::Int32,
            .bufferSize = 512,
        };

        ASSERT_TRUE(player.setProcessFunction(playbackCallback));
        ASSERT_TRUE(player.open(playbackConfig));
        ASSERT_TRUE(player.start());

        while (g_playbackFrames.load() < g_recordCapacityFrames) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        ASSERT_TRUE(player.stop());
        ASSERT_TRUE(player.close());
    }
}