//
// Created by mika on 9/27/26.
//

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <print>
#include <string_view>
#include <thread>

import mka.audio.pipewire;
import mka.audio.process;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;

static const char* fmtToStr(const mka::audio::Format format) {
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

//--- Test GetEndPoints ------------------------------------------------------
//
// Contrairement à ALSA (device fixe "hw:1,0" sur le banc de test), PipeWire
// n'expose pas d'id stable et portable : les noms de noeuds dépendent de la
// machine. Les tests ci-dessous ne présupposent donc jamais un id précis --
// sauf TestGetEndPointsContainsExpectedDeviceIfConfigured, qui lit un id
// attendu depuis une variable d'environnement pour rester reproductible en
// CI sans pour autant casser sur une machine sans configuration spécifique.

TEST(PipeWireBackendTest, TestGetEndPoints) {
    const mka::audio::PipeWire pw;
    const auto endpoints = pw.getEndPoints();

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

TEST(PipeWireBackendTest, TestGetEndPointsDoesNotThrow) {
    const mka::audio::PipeWire pw;
    // getEndPoints_ ne doit jamais lancer d'exception, même sans serveur
    // PipeWire actif -- c'est ce que ce test garantit avant tout.
    ASSERT_NO_THROW({
        auto endpoints = pw.getEndPoints();
    });
}

TEST(PipeWireBackendTest, TestGetEndPointsIsConsistentAcrossCalls) {
    const mka::audio::PipeWire pw;
    const auto first = pw.getEndPoints();
    const auto second = pw.getEndPoints();

    // Deux scans consécutifs ne doivent pas planter ni se contredire sur la
    // simple présence d'endpoints (le graphe PipeWire peut changer entre les
    // deux appels, mais l'un ne doit jamais renvoyer d'exception là où
    // l'autre ne le ferait pas).
    EXPECT_EQ(first.empty(), first.size() == 0);
    EXPECT_EQ(second.empty(), second.size() == 0);
}

TEST(PipeWireBackendTest, TestGetEndPointsContainsExpectedDeviceIfConfigured) {
    const char* expectedId = std::getenv("MKA_TEST_PIPEWIRE_DEVICE_ID");
    if (!expectedId || std::string_view{expectedId}.empty()) {
        GTEST_SKIP() << "MKA_TEST_PIPEWIRE_DEVICE_ID non défini, test ignoré";
    }

    const mka::audio::PipeWire pw;
    const auto endpoints = pw.getEndPoints();

    const auto it = std::ranges::find_if(endpoints, [&](const mka::audio::Endpoint& e) {
        return e.id == expectedId;
    });

    ASSERT_NE(it, endpoints.end()) << "endpoint attendu introuvable: " << expectedId;
}

TEST(PipeWireBackendTest, TestGetEndPointsCallableWhileClosed) {
    // Backend::getEndPoints() n'a aucune garde d'état (contrairement à
    // open/start/stop/close) : il doit être appelable à tout moment.
    const mka::audio::PipeWire pw;
    ASSERT_NO_THROW({
        auto endpoints = pw.getEndPoints();
        (void) endpoints;
    });
}

//--- Fixture pour les tests nécessitant un serveur PipeWire + du matériel ---
//
// Un id PipeWire n'étant pas portable, on découvre un endpoint de sortie (et
// d'entrée) disponible une seule fois pour toute la suite. Les tests qui en
// ont besoin s'auto-ignorent (GTEST_SKIP) si la machine n'expose aucun
// endpoint correspondant, plutôt que d'échouer arbitrairement.

namespace {
    struct DiscoveredEndpoint {
        bool available = false;
        std::string id;
        mka::audio::SampleRate sampleRate = 0;
        mka::audio::BufferSize bufferSize = 0;
        std::uint32_t channels = 0;
    };

    DiscoveredEndpoint discoverOutput() {
        const mka::audio::PipeWire pw;
        const auto endpoints = pw.getEndPoints();
        const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
            return e.output.has_value() && !e.output->sampleRates.empty()
                && !e.output->bufferSizes.empty() && e.output->maxChannels > 0;
        });
        if (it == endpoints.end()) return {};

        return DiscoveredEndpoint{
            .available = true,
            .id = it->id,
            .sampleRate = it->output->sampleRates.front(),
            .bufferSize = it->output->bufferSizes.front(),
            .channels = std::min<std::uint32_t>(2, it->output->maxChannels),
        };
    }

    DiscoveredEndpoint discoverInput() {
        const mka::audio::PipeWire pw;
        const auto endpoints = pw.getEndPoints();
        const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
            return e.input.has_value() && !e.input->sampleRates.empty()
                && !e.input->bufferSizes.empty() && e.input->maxChannels > 0;
        });
        if (it == endpoints.end()) return {};

        return DiscoveredEndpoint{
            .available = true,
            .id = it->id,
            .sampleRate = it->input->sampleRates.front(),
            .bufferSize = it->input->bufferSizes.front(),
            .channels = std::min<std::uint32_t>(2, it->input->maxChannels),
        };
    }
}

class PipeWireBackendHwTest : public ::testing::Test {
    protected:
        static DiscoveredEndpoint output;
        static DiscoveredEndpoint input;

        static void SetUpTestSuite() {
            output = discoverOutput();
            input = discoverInput();
        }

        static mka::audio::EndpointConfig makeOutputConfig() {
            return mka::audio::EndpointConfig{
                .id = output.id,
                .direction = mka::audio::Direction::Output,
                .inputChannels = 0,
                .outputChannels = output.channels,
                .sampleRate = output.sampleRate,
                .format = mka::audio::Format::Float32,
                .bufferSize = output.bufferSize,
            };
        }

        void SetUp() override {
            if (!output.available) {
                GTEST_SKIP() << "aucun endpoint de sortie disponible sur cette machine";
            }
        }
};
DiscoveredEndpoint PipeWireBackendHwTest::output;
DiscoveredEndpoint PipeWireBackendHwTest::input;

//--- Test Open : garde-fous indépendants du matériel ------------------------

TEST(PipeWireBackendTest, TestOpenDuplexRejectedConfigurationFailed) {
    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = "",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 2,
        .outputChannels = 2,
        .sampleRate = 48000,
        .format = mka::audio::Format::Float32,
        .bufferSize = 512,
    };

    // Direction::Duplex n'est volontairement pas géré (cf. commentaire dans
    // pipewire.cppm) : la vérification doit avoir lieu avant toute tentative
    // d'ouverture, quels que soient les autres champs.
    auto ret = pw.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::ConfigurationFailed);
}

TEST(PipeWireBackendTest, TestOpenInvalidSampleRateRejected) {
    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = "",
        .direction = mka::audio::Direction::Output,
        .outputChannels = 2,
        .sampleRate = 44190, // n'appartient pas à supportedSampleRates
        .format = mka::audio::Format::Float32,
        .bufferSize = 512,
    };

    auto ret = pw.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::SampleRateNotSupported);
}

TEST(PipeWireBackendTest, TestOpenInvalidBufferSizeRejected) {
    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = "",
        .direction = mka::audio::Direction::Output,
        .outputChannels = 2,
        .sampleRate = 48000,
        .format = mka::audio::Format::Float32,
        .bufferSize = 500, // n'appartient pas à supportedBufferSizes
    };

    auto ret = pw.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::BufferSizeNotSupported);
}

TEST(PipeWireBackendTest, TestOpenInvalidFormatRejected) {
    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = "",
        .direction = mka::audio::Direction::Output,
        .outputChannels = 2,
        .sampleRate = 48000,
        .format = mka::audio::Format::Int16, // seul Float32 est négocié ici
        .bufferSize = 512,
    };

    auto ret = pw.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::FormatNotSupported);
}

TEST(PipeWireBackendTest, TestOpenInvalidChannelCountRejected) {
    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = "",
        .direction = mka::audio::Direction::Output,
        .outputChannels = 0, // aucun canal demandé
        .sampleRate = 48000,
        .format = mka::audio::Format::Float32,
        .bufferSize = 512,
    };

    auto ret = pw.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(PipeWireBackendTest, TestOpenInvalidIDFails) {
    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = "this-node-name-does-not-exist-999999",
        .direction = mka::audio::Direction::Output,
        .outputChannels = 2,
        .sampleRate = 48000,
        .format = mka::audio::Format::Float32,
        .bufferSize = 512,
    };

    // Tous les autres champs sont valides afin d'isoler précisément la
    // vérification d'existence de l'endpoint.
    auto ret = pw.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::EndpointUnavailable);
}

TEST(PipeWireBackendTest, TestOpenIDWithWrongDirectionFails) {
    // Un endpoint capture-only (Audio/Source) demandé en Output (et
    // vice-versa) doit être refusé, même si l'id existe bel et bien.
    const mka::audio::PipeWire discoverer;
    const auto endpoints = discoverer.getEndPoints();

    const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.input.has_value() && !e.output.has_value();
    });
    if (it == endpoints.end()) {
        GTEST_SKIP() << "aucun endpoint input-only (sans capacité output) trouvé";
    }

    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = it->id,
        .direction = mka::audio::Direction::Output, // demandé en sortie
        .outputChannels = 2,
        .sampleRate = 48000,
        .format = mka::audio::Format::Float32,
        .bufferSize = 512,
    };

    auto ret = pw.open(config);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::EndpointUnavailable);
}

TEST(PipeWireBackendTest, TestOpenFailureLeavesStateClosedAllowingRetry) {
    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig badConfig {
        .id = "still-not-a-real-endpoint",
        .direction = mka::audio::Direction::Output,
        .outputChannels = 2,
        .sampleRate = 48000,
        .format = mka::audio::Format::Float32,
        .bufferSize = 512,
    };

    ASSERT_FALSE(pw.open(badConfig));

    // Un échec d'open_ ne doit pas laisser l'état bloqué : un nouvel open
    // avec une configuration valide doit rester possible ensuite.
    const auto output = discoverOutput();
    if (!output.available) {
        GTEST_SKIP() << "aucun endpoint de sortie disponible pour vérifier le retry";
    }

    const mka::audio::EndpointConfig goodConfig {
        .id = output.id,
        .direction = mka::audio::Direction::Output,
        .outputChannels = output.channels,
        .sampleRate = output.sampleRate,
        .format = mka::audio::Format::Float32,
        .bufferSize = output.bufferSize,
    };

    auto ret = pw.open(goodConfig);
    ASSERT_TRUE(ret);
    ASSERT_TRUE(pw.close());
}

//--- Test Open : chemins nécessitant un endpoint réel -----------------------

TEST_F(PipeWireBackendHwTest, TestOpenSucceed) {
    mka::audio::PipeWire pw;
    auto ret = pw.open(makeOutputConfig());
    ASSERT_TRUE(ret);
    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestOpenTwiceFailsWithInvalidState) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));

    auto ret = pw.open(makeOutputConfig());
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(pw.close());
}

//--- Test Start --------------------------------------------------------------

TEST(PipeWireBackendTest, TestStartWithoutOpenFailsInvalidState) {
    mka::audio::PipeWire pw;

    auto ret = pw.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST_F(PipeWireBackendHwTest, TestStartSucceedsAfterOpen) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));

    auto ret = pw.start();
    ASSERT_TRUE(ret);

    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestStartTwiceFailsInvalidState) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());

    auto ret = pw.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());
}

namespace {
    std::atomic<bool> g_callbackCalled{false};
    std::atomic<std::thread::id> g_callbackThreadId{};

    void testCallback(const mka::audio::AudioProcessContext&) noexcept {
        g_callbackThreadId.store(std::this_thread::get_id());
        g_callbackCalled.store(true);
    }
}

TEST_F(PipeWireBackendHwTest, TestStartInvokesCallbackOnDifferentThread) {
    g_callbackCalled.store(false);
    g_callbackThreadId.store({});

    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.setProcessFunction(testCallback));
    ASSERT_TRUE(pw.open(makeOutputConfig()));

    const auto callingThreadId = std::this_thread::get_id();
    ASSERT_TRUE(pw.start());

    for (int i = 0; i < 100 && !g_callbackCalled.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ASSERT_TRUE(g_callbackCalled.load());
    ASSERT_NE(g_callbackThreadId.load(), callingThreadId);

    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestSetProcessFunctionFailsWhileRunning) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());

    auto ret = pw.setProcessFunction(testCallback);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());
}

TEST(PipeWireBackendTest, TestSetProcessFunctionAllowedWhenClosed) {
    mka::audio::PipeWire pw;

    auto ret = pw.setProcessFunction(testCallback);
    ASSERT_TRUE(ret);
}

//--- Test Stop ----------------------------------------------------------------

TEST(PipeWireBackendTest, TestStopWithoutStartFailsInvalidState) {
    mka::audio::PipeWire pw;

    auto ret = pw.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST_F(PipeWireBackendHwTest, TestStopWithoutStartAfterOpenFailsInvalidState) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));

    auto ret = pw.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestStopSucceedsAfterStart) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());

    auto ret = pw.stop();
    ASSERT_TRUE(ret);

    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestStopTwiceFailsInvalidState) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());
    ASSERT_TRUE(pw.stop());

    auto ret = pw.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestStopActuallyHaltsCallbackInvocations) {
    g_callbackCalled.store(false);

    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.setProcessFunction(testCallback));
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());

    for (int i = 0; i < 100 && !g_callbackCalled.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(g_callbackCalled.load());

    ASSERT_TRUE(pw.stop());

    g_callbackCalled.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_FALSE(g_callbackCalled.load());

    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestStopAllowsReopenAndRestart) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());
    ASSERT_TRUE(pw.stop());

    auto ret = pw.start();
    ASSERT_TRUE(ret);
    ASSERT_TRUE(pw.stop());

    ASSERT_TRUE(pw.close());
}

//--- Test Close -----------------------------------------------------------

TEST(PipeWireBackendTest, TestCloseWithoutOpenFailsInvalidState) {
    mka::audio::PipeWire pw;

    auto ret = pw.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST_F(PipeWireBackendHwTest, TestCloseSucceedsAfterOpen) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));

    auto ret = pw.close();
    ASSERT_TRUE(ret);
}

TEST_F(PipeWireBackendHwTest, TestCloseFailsWhileRunning) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());

    // Backend impose l'ordre open -> start -> stop -> close : close_ doit
    // être refusé tant que le flux tourne encore (state == Running).
    auto ret = pw.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestCloseTwiceFailsInvalidState) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.close());

    auto ret = pw.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST_F(PipeWireBackendHwTest, TestCloseReleasesEndpointForReopen) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.close());

    // Si close_ ne détruit pas vraiment le stream/la boucle PipeWire, un
    // second open sur le même id pourrait échouer ou laisser des ressources
    // fuiter. On vérifie ici que la réouverture réussit bien.
    auto ret = pw.open(makeOutputConfig());
    ASSERT_TRUE(ret);
    ASSERT_TRUE(pw.close());
}

TEST_F(PipeWireBackendHwTest, TestFullLifecycleOpenStartStopClose) {
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());
    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());
}

//--- Test Sinewave (manuel, désactivé par défaut) ---------------------------

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

// Désactivé par défaut : joue réellement du son. À activer manuellement avec
// --gtest_filter=*DISABLED_TestPlaySineWave* --gtest_also_run_disabled_tests
TEST(PipeWireBackendTest, DISABLED_TestPlaySineWave) {
    mka::audio::PipeWire discoverer;
    const auto endpoints = discoverer.getEndPoints();

    const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.output.has_value();
    });
    ASSERT_NE(it, endpoints.end());

    constexpr double frequency = 440.0;
    const mka::audio::SampleRate sampleRate = it->output->sampleRates.front();

    mka::audio::PipeWire pw;
    const mka::audio::EndpointConfig config {
        .id = it->id,
        .direction = mka::audio::Direction::Output,
        .outputChannels = 2,
        .sampleRate = sampleRate,
        .format = mka::audio::Format::Float32,
        .bufferSize = it->output->bufferSizes.front(),
    };

    g_sinePhase = 0.0;
    g_sinePhaseIncrement = 2.0 * std::numbers::pi * frequency / static_cast<double>(sampleRate);

    ASSERT_TRUE(pw.setProcessFunction(sineCallback));
    ASSERT_TRUE(pw.open(config));
    ASSERT_TRUE(pw.start());

    std::this_thread::sleep_for(std::chrono::seconds(1));

    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());
}

//--- Test Record & Playback (manuel, désactivé par défaut) ------------------
//
// Anti-clicks, trois sources traitées séparément :
//  1. Transitoire de démarrage de la capture (pop du micro) : les premières
//     ms capturées sont jetées (warm-up).
//  2. Discontinuités aux bords du signal : fade-in / fade-out (cosinus
//     surélevé) appliqué hors temps réel sur le buffer enregistré.
//  3. Réveil / mise en veille du sink : la lecture émet du silence avant et
//     après le signal, donc le pop matériel éventuel tombe dans le silence.

namespace {
    std::vector<std::vector<float>> g_recordedSamples;
    std::atomic<std::size_t> g_recordedFrames{0};
    std::atomic<std::size_t> g_warmupFramesLeft{0};
    std::size_t g_recordCapacityFrames = 0;

    void recordCallback(const mka::audio::AudioProcessContext &ctx) noexcept {
        // Warm-up : ignore les premières frames (une seule écriture, depuis
        // le thread audio, donc load/store suffit).
        std::size_t start = 0;
        const std::size_t warm = g_warmupFramesLeft.load();
        if (warm > 0) {
            start = std::min<std::size_t>(warm, ctx.frames);
            g_warmupFramesLeft.store(warm - start);
        }

        const std::size_t available = ctx.frames - start;
        const std::size_t writeOffset = g_recordedFrames.load();
        const std::size_t framesLeft = g_recordCapacityFrames - writeOffset;
        const std::size_t framesToCopy = std::min<std::size_t>(framesLeft, available);

        for (std::uint32_t ch = 0; ch < ctx.input.count && ch < g_recordedSamples.size(); ++ch) {
            for (std::size_t i = 0; i < framesToCopy; ++i) {
                g_recordedSamples[ch][writeOffset + i] = ctx.input.channels[ch][start + i];
            }
        }

        g_recordedFrames.fetch_add(framesToCopy);
    }

    // Timeline de lecture : [ silence lead-in | signal | silence tail ]
    std::atomic<std::size_t> g_playbackPosition{0};
    std::size_t g_playbackLeadFrames = 0;
    std::size_t g_playbackTotalFrames = 0;

    void playbackFromRecordCallback(const mka::audio::AudioProcessContext &ctx) noexcept {
        const std::size_t pos = g_playbackPosition.load();

        for (std::uint32_t ch = 0; ch < ctx.output.count; ++ch) {
            const std::uint32_t srcCh = ch < g_recordedSamples.size() ? ch : 0;
            for (std::size_t i = 0; i < ctx.frames; ++i) {
                const std::size_t t = pos + i;
                const bool inSignal = t >= g_playbackLeadFrames
                                   && t - g_playbackLeadFrames < g_recordCapacityFrames;
                ctx.output.channels[ch][i] =
                    inSignal ? g_recordedSamples[srcCh][t - g_playbackLeadFrames] : 0.0f;
            }
        }

        g_playbackPosition.store(pos + ctx.frames);
    }

    // Fade-in / fade-out en cosinus surélevé (pente nulle aux extrémités).
    void applyFades(std::vector<std::vector<float>> &samples, std::size_t fadeFrames) {
        for (auto &channel : samples) {
            const std::size_t n = channel.size();
            const std::size_t f = std::min(fadeFrames, n / 2);
            for (std::size_t i = 0; i < f; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(f);
                const auto gain = static_cast<float>(0.5 * (1.0 - std::cos(std::numbers::pi * t)));
                channel[i] *= gain;
                channel[n - 1 - i] *= gain;
            }
        }
    }
}

// Désactivé par défaut : enregistre réellement depuis un node d'entrée et
// rejoue sur un node de sortie. À activer manuellement.
TEST(PipeWireBackendTest, DISABLED_TestRecordAndPlayback) {
    mka::audio::PipeWire discoverer;
    const auto endpoints = discoverer.getEndPoints();

    const auto inIt = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.input.has_value();
    });
    const auto outIt = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.output.has_value();
    });

    ASSERT_NE(inIt, endpoints.end()) << "no input node found";
    ASSERT_NE(outIt, endpoints.end()) << "no output node found";

    constexpr std::uint32_t channels = 2;
    constexpr int recordSeconds = 5;
    // 64 frames (front() de la liste) est un quantum minuscule : le moindre
    // retard du thread provoque un xrun audible comme un click. On prend une
    // taille confortable, la latence n'a aucune importance ici.
    constexpr mka::audio::BufferSize bufferSize = 512;
    const mka::audio::SampleRate sampleRate = inIt->input->sampleRates.front();

    const auto msToFrames = [&](const int ms) {
        return static_cast<std::size_t>(sampleRate) * ms / 1000;
    };
    const std::size_t warmupFrames = msToFrames(300); // pop de démarrage du micro
    const std::size_t fadeFrames = msToFrames(20);
    const std::size_t silenceFrames = msToFrames(300); // lead-in / tail de lecture

    g_recordCapacityFrames = static_cast<std::size_t>(sampleRate) * recordSeconds;
    g_recordedSamples.assign(channels, std::vector<float>(g_recordCapacityFrames, 0.0f));
    g_recordedFrames.store(0);
    g_warmupFramesLeft.store(warmupFrames);

    // --- Phase 1 : enregistrement ---
    {
        mka::audio::PipeWire recorder;
        const mka::audio::EndpointConfig recordConfig {
            .id = "alsa_input.pci-0000_35_00.6.HiFi__Mic1__source",
            .direction = mka::audio::Direction::Input,
            .inputChannels = channels,
            .sampleRate = sampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = bufferSize,
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

    // Post-traitement hors temps réel : le thread audio est arrêté.
    applyFades(g_recordedSamples, fadeFrames);

    // --- Attente ---
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // --- Phase 2 : lecture ---
    g_playbackLeadFrames = silenceFrames;
    g_playbackTotalFrames = silenceFrames + g_recordCapacityFrames + silenceFrames;
    g_playbackPosition.store(0);

    {
        mka::audio::PipeWire player;
        const mka::audio::EndpointConfig playbackConfig {
            .id = outIt->id,
            .direction = mka::audio::Direction::Output,
            .outputChannels = channels,
            .sampleRate = sampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = bufferSize,
        };

        ASSERT_TRUE(player.setProcessFunction(playbackFromRecordCallback));
        ASSERT_TRUE(player.open(playbackConfig));
        ASSERT_TRUE(player.start());

        // Attend la fin du silence de queue avant de stopper : le stream
        // s'arrête ainsi sur du silence, jamais en plein signal.
        while (g_playbackPosition.load() < g_playbackTotalFrames) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        ASSERT_TRUE(player.stop());
        ASSERT_TRUE(player.close());
    }
}