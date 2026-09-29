//
// Tests du backend PulseAudio (mka.audio.pulseaudio)
//
// Organisation, identique aux suites PipeWire/JACK :
//  - PulseAudioBackendTest   : sans dépendance à un serveur (garde-fous
//                              locaux, états). Passent avec ou sans serveur.
//  - PulseAudioBackendHwTest : nécessitent un serveur PulseAudio actif. Les
//                              endpoints sont découverts au lancement de la
//                              suite ; chaque test s'ignore (GTEST_SKIP) si
//                              l'endpoint dont il a besoin manque.
//
// Différence structurante avec JACK : PulseAudio rééchantillonne et adapte
// la taille de buffer automatiquement, donc n'importe quelle combinaison
// valide de mka.audio.constants fonctionne quel que soit le serveur -- pas
// besoin de découvrir le rate/buffer "imposé" comme pour JACK.
//
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

import mka.audio.pulseaudio;
import mka.audio.process;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;

//--- Utilitaires ------------------------------------------------------------

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

namespace {
    // Config de référence : rate/buffer choisis dans les constantes globales.
    // Contrairement à JACK, PulseAudio les accepte tous quel que soit le
    // serveur (il rééchantillonne / adapte), donc aucune découverte n'est
    // nécessaire pour ces deux champs.
    constexpr mka::audio::SampleRate kSampleRate = 48000;
    constexpr mka::audio::BufferSize kBufferSize = 512;

    mka::audio::EndpointConfig validOutputConfig(std::string id = "") {
        return mka::audio::EndpointConfig{
            .id = std::move(id),
            .direction = mka::audio::Direction::Output,
            .inputChannels = 0,
            .outputChannels = 2,
            .sampleRate = kSampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = kBufferSize,
        };
    }

    void expectOpenError(mka::audio::PulseAudio& pa, const mka::audio::EndpointConfig& cfg, const mka::audio::ErrorType expected) {
        const auto ret = pa.open(cfg);
        ASSERT_FALSE(ret) << "open() aurait dû échouer";
        EXPECT_EQ(ret.error(), expected);
    }

    template <class Pred>
    bool waitFor(Pred pred, const int timeoutMs = 2000) {
        for (int waited = 0; waited < timeoutMs; waited += 20) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    }

    // Callbacks de test (thread audio Pulse).
    std::atomic<bool> g_callbackCalled{false};
    std::atomic<std::thread::id> g_callbackThreadId{};
    std::atomic<std::uint32_t> g_inCount{0};
    std::atomic<std::uint32_t> g_outCount{0};
    std::atomic<std::uint32_t> g_frames{0};
    std::atomic<bool> g_pointersValid{false};

    void resetCallbackState() {
        g_callbackCalled.store(false);
        g_callbackThreadId.store({});
        g_inCount.store(0);
        g_outCount.store(0);
        g_frames.store(0);
        g_pointersValid.store(false);
    }

    void silence(const mka::audio::AudioProcessContext& ctx) noexcept {
        for (std::uint32_t ch = 0; ch < ctx.output.count; ++ch) {
            std::fill_n(ctx.output.channels[ch], ctx.frames, 0.0f);
        }
    }

    void testCallback(const mka::audio::AudioProcessContext& ctx) noexcept {
        g_callbackThreadId.store(std::this_thread::get_id());
        g_callbackCalled.store(true);
        silence(ctx);
    }

    // Enregistre ce que le backend fournit réellement au callback.
    void inspectCallback(const mka::audio::AudioProcessContext& ctx) noexcept {
        bool valid = true;
        for (std::uint32_t ch = 0; ch < ctx.input.count; ++ch) valid = valid && ctx.input.channels[ch] != nullptr;
        for (std::uint32_t ch = 0; ch < ctx.output.count; ++ch) valid = valid && ctx.output.channels[ch] != nullptr;

        g_inCount.store(ctx.input.count);
        g_outCount.store(ctx.output.count);
        g_frames.store(ctx.frames);
        g_pointersValid.store(valid);
        g_callbackCalled.store(true);
        silence(ctx);
    }
}

//--- Test GetEndPoints ------------------------------------------------------

TEST(PulseAudioBackendTest, TestGetEndPoints) {
    const mka::audio::PulseAudio pa;
    const auto endpoints = pa.getEndPoints();

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

TEST(PulseAudioBackendTest, TestGetEndPointsDoesNotThrow) {
    const mka::audio::PulseAudio pa;
    // Contrat : jamais d'exception, y compris sans serveur PulseAudio actif.
    ASSERT_NO_THROW({
        auto endpoints = pa.getEndPoints();
    });
}

TEST(PulseAudioBackendTest, TestGetEndPointsCallableWhileClosed) {
    // Backend::getEndPoints() n'a aucune garde d'état : appelable à tout moment.
    const mka::audio::PulseAudio pa;
    ASSERT_NO_THROW({
        auto endpoints = pa.getEndPoints();
        (void) endpoints;
    });
}

TEST(PulseAudioBackendTest, TestGetEndPointsIdsAreUnique) {
    const mka::audio::PulseAudio pa;
    const auto endpoints = pa.getEndPoints();

    std::vector<std::string> ids;
    for (const auto& e : endpoints) ids.push_back(e.id);
    std::ranges::sort(ids);

    EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end()) << "id d'endpoint en double";
}

TEST(PulseAudioBackendTest, TestGetEndPointsCapabilitiesReflectServer) {
    const mka::audio::PulseAudio pa;
    const auto endpoints = pa.getEndPoints();
    if (endpoints.empty()) {
        GTEST_SKIP() << "aucun endpoint (serveur PulseAudio absent ?)";
    }

    const auto check = [](const mka::audio::StreamCapabilities& caps) {
        EXPECT_EQ(caps.minChannels, 1u);
        EXPECT_GE(caps.maxChannels, 1u);

        // Format toujours Float32 (seul format réellement délivré).
        ASSERT_EQ(caps.formats.size(), 1u);
        EXPECT_TRUE(caps.formats.front() == mka::audio::Format::Float32);

        // Contrairement à JACK, le serveur accepte toutes les valeurs
        // globales : les listes complètes doivent être reflétées telles quelles.
        EXPECT_EQ(caps.sampleRates.size(), mka::audio::supportedSampleRates.size());
        EXPECT_EQ(caps.bufferSizes.size(), mka::audio::supportedBufferSizes.size());
    };

    for (const auto& endpoint : endpoints) {
        if (endpoint.input) check(*endpoint.input);
        if (endpoint.output) check(*endpoint.output);
    }
}

TEST(PulseAudioBackendTest, TestGetEndPointsContainsExpectedDeviceIfConfigured) {
    const char* expectedId = std::getenv("MKA_TEST_PULSEAUDIO_DEVICE_ID");
    if (!expectedId || std::string_view{expectedId}.empty()) {
        GTEST_SKIP() << "MKA_TEST_PULSEAUDIO_DEVICE_ID non défini, test ignoré";
    }

    const mka::audio::PulseAudio pa;
    const auto endpoints = pa.getEndPoints();

    const auto it = std::ranges::find_if(endpoints, [&](const mka::audio::Endpoint& e) {
        return e.id == expectedId;
    });
    ASSERT_NE(it, endpoints.end()) << "endpoint attendu introuvable: " << expectedId;
}

//--- Test Open : garde-fous indépendants du serveur -------------------------

TEST(PulseAudioBackendTest, TestOpenDuplexRejectedConfigurationFailed) {
    // Un flux PulseAudio est unidirectionnel : Duplex n'est pas géré (cf.
    // note en tête de pulseaudio.cppm), quels que soient les autres champs.
    mka::audio::PulseAudio pa;
    const mka::audio::EndpointConfig config {
        .id = "",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 2,
        .outputChannels = 2,
        .sampleRate = kSampleRate,
        .format = mka::audio::Format::Float32,
        .bufferSize = kBufferSize,
    };
    expectOpenError(pa, config, mka::audio::ErrorType::ConfigurationFailed);
}

TEST(PulseAudioBackendTest, TestOpenInvalidSampleRateRejected) {
    mka::audio::PulseAudio pa;
    auto cfg = validOutputConfig();
    cfg.sampleRate = 44190; // n'appartient pas à supportedSampleRates
    expectOpenError(pa, cfg, mka::audio::ErrorType::SampleRateNotSupported);
}

TEST(PulseAudioBackendTest, TestOpenInvalidBufferSizeRejected) {
    mka::audio::PulseAudio pa;
    auto cfg = validOutputConfig();
    cfg.bufferSize = 500; // n'appartient pas à supportedBufferSizes
    expectOpenError(pa, cfg, mka::audio::ErrorType::BufferSizeNotSupported);
}

TEST(PulseAudioBackendTest, TestOpenInvalidFormatRejected) {
    // Seul Float32 est réellement délivré (PA_SAMPLE_FLOAT32NE), même si
    // les autres formats figurent dans supportedFormats (liste globale,
    // tous backends confondus).
    mka::audio::PulseAudio pa;
    for (const auto format : mka::audio::supportedFormats) {
        if (format == mka::audio::Format::Float32) continue;
        SCOPED_TRACE(fmtToStr(format));

        auto cfg = validOutputConfig();
        cfg.format = format;
        expectOpenError(pa, cfg, mka::audio::ErrorType::FormatNotSupported);
    }
}

TEST(PulseAudioBackendTest, TestOpenOutputZeroChannelsRejected) {
    mka::audio::PulseAudio pa;
    auto cfg = validOutputConfig();
    cfg.outputChannels = 0;
    expectOpenError(pa, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(PulseAudioBackendTest, TestOpenInputZeroChannelsRejected) {
    mka::audio::PulseAudio pa;
    mka::audio::EndpointConfig cfg {
        .id = "",
        .direction = mka::audio::Direction::Input,
        .inputChannels = 0,
        .outputChannels = 2, // ignoré pour une direction Input
        .sampleRate = kSampleRate,
        .format = mka::audio::Format::Float32,
        .bufferSize = kBufferSize,
    };
    expectOpenError(pa, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(PulseAudioBackendTest, TestOpenTooManyChannelsRejected) {
    // pa_sample_spec::channels est un uint8_t : au-delà de 255, la config
    // ne peut de toute façon pas être représentée fidèlement.
    mka::audio::PulseAudio pa;
    auto cfg = validOutputConfig();
    cfg.outputChannels = 256;
    expectOpenError(pa, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(PulseAudioBackendTest, TestOpenValidationErrorPrecedence) {
    // Ordre des vérifications locales : direction, rate, buffer, format, channels.
    mka::audio::PulseAudio pa;
    mka::audio::EndpointConfig cfg{
        .id = "",
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = 0,
        .outputChannels = 0,
        .sampleRate = 44190,
        .format = mka::audio::Format::Int16,
        .bufferSize = 500,
    };

    expectOpenError(pa, cfg, mka::audio::ErrorType::ConfigurationFailed);
    cfg.direction = mka::audio::Direction::Output;
    expectOpenError(pa, cfg, mka::audio::ErrorType::SampleRateNotSupported);
    cfg.sampleRate = kSampleRate;
    expectOpenError(pa, cfg, mka::audio::ErrorType::BufferSizeNotSupported);
    cfg.bufferSize = kBufferSize;
    expectOpenError(pa, cfg, mka::audio::ErrorType::FormatNotSupported);
    cfg.format = mka::audio::Format::Float32;
    expectOpenError(pa, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(PulseAudioBackendTest, TestOpenInvalidIDFails) {
    // Vrai avec ou sans serveur : sans serveur, l'endpoint est de toute façon
    // indisponible. Tous les autres champs sont valides pour isoler l'id.
    mka::audio::PulseAudio pa;
    expectOpenError(pa, validOutputConfig("this-device-does-not-exist-999999"), mka::audio::ErrorType::EndpointUnavailable);
}

//--- Test Start / Stop / Close : garde-fous d'état --------------------------

TEST(PulseAudioBackendTest, TestStartWithoutOpenFailsInvalidState) {
    mka::audio::PulseAudio pa;

    auto ret = pa.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(PulseAudioBackendTest, TestStopWithoutStartFailsInvalidState) {
    mka::audio::PulseAudio pa;

    auto ret = pa.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(PulseAudioBackendTest, TestCloseWithoutOpenFailsInvalidState) {
    mka::audio::PulseAudio pa;

    auto ret = pa.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(PulseAudioBackendTest, TestSetProcessFunctionAllowedWhenClosed) {
    mka::audio::PulseAudio pa;

    auto ret = pa.setProcessFunction(testCallback);
    ASSERT_TRUE(ret);
}

TEST(PulseAudioBackendTest, TestFailedOpenDoesNotChangeState) {
    // Après un open échoué, l'état reste Closed : start/close refusés.
    mka::audio::PulseAudio pa;
    auto cfg = validOutputConfig();
    cfg.format = mka::audio::Format::Int16;
    expectOpenError(pa, cfg, mka::audio::ErrorType::FormatNotSupported);

    auto start = pa.start();
    ASSERT_FALSE(start);
    EXPECT_EQ(start.error(), mka::audio::ErrorType::InvalidState);

    auto close = pa.close();
    ASSERT_FALSE(close);
    EXPECT_EQ(close.error(), mka::audio::ErrorType::InvalidState);
}

//--- Fixture pour les tests nécessitant un serveur PulseAudio ---------------

namespace {
    struct DiscoveredEndpoint {
        bool available = false;
        std::string id;
    };

    DiscoveredEndpoint discoverOutput() {
        const mka::audio::PulseAudio pa;
        const auto endpoints = pa.getEndPoints();
        const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
            return e.output.has_value();
        });
        if (it == endpoints.end()) return {};
        return DiscoveredEndpoint{ .available = true, .id = it->id };
    }

    DiscoveredEndpoint discoverInput() {
        const mka::audio::PulseAudio pa;
        const auto endpoints = pa.getEndPoints();

        // Motif voulu : variable d'environnement, sinon "Mic1" par défaut.
        const char* env = std::getenv("MKA_TEST_PULSEAUDIO_INPUT_HINT");
        const std::string hint = env ? env : "Mic1";

        // 1) Entrée dont l'id contient le motif
        auto it = std::ranges::find_if(endpoints, [&](const mka::audio::Endpoint& e) {
            return e.input.has_value() && e.id.find(hint) != std::string::npos;
        });

        // 2) Sinon, première entrée disponible
        if (it == endpoints.end()) {
            it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
                return e.input.has_value();
            });
        }

        if (it == endpoints.end()) return {};
        return DiscoveredEndpoint{ .available = true, .id = it->id };
    }
}

#define SKIP_UNLESS(ep) \
    do { \
        if (!(ep).available) { \
            GTEST_SKIP() << "endpoint '" #ep "' indisponible (serveur PulseAudio absent ?)"; \
        } \
    } while (0)

class PulseAudioBackendHwTest : public ::testing::Test {
    protected:
        static DiscoveredEndpoint output;
        static DiscoveredEndpoint input;

        static void SetUpTestSuite() {
            output = discoverOutput();
            input = discoverInput();
        }

        static mka::audio::EndpointConfig makeOutputConfig() {
            auto cfg = validOutputConfig(output.id);
            return cfg;
        }

        static mka::audio::EndpointConfig makeInputConfig() {
            return mka::audio::EndpointConfig{
                .id = input.id,
                .direction = mka::audio::Direction::Input,
                .inputChannels = 2,
                .outputChannels = 0,
                .sampleRate = kSampleRate,
                .format = mka::audio::Format::Float32,
                .bufferSize = kBufferSize,
            };
        }
};
DiscoveredEndpoint PulseAudioBackendHwTest::output;
DiscoveredEndpoint PulseAudioBackendHwTest::input;

//--- Test GetEndPoints avec serveur -----------------------------------------

TEST_F(PulseAudioBackendHwTest, TestGetEndPointsCallableWhileRunning) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.setProcessFunction(testCallback));
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());

    ASSERT_NO_THROW({
        const auto endpoints = pa.getEndPoints();
        EXPECT_FALSE(endpoints.empty());
    });

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

//--- Test Open : erreurs dépendant du serveur ------------------------------

TEST_F(PulseAudioBackendHwTest, TestOpenIDWithWrongDirectionFails) {
    // Un endpoint existant mais sans la capacité demandée (source-only en
    // Output, ou l'inverse) doit être refusé.
    SKIP_UNLESS(output);

    const mka::audio::PulseAudio discoverer;
    const auto endpoints = discoverer.getEndPoints();
    const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.input.has_value() && !e.output.has_value();
    });
    if (it == endpoints.end()) GTEST_SKIP() << "aucun endpoint source-only trouvé";

    mka::audio::PulseAudio pa;
    expectOpenError(pa, validOutputConfig(it->id), mka::audio::ErrorType::EndpointUnavailable);
}

TEST_F(PulseAudioBackendHwTest, TestOpenFailureLeavesStateClosedAllowingRetry) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    expectOpenError(pa, validOutputConfig("still-not-a-real-device"), mka::audio::ErrorType::EndpointUnavailable);

    // L'échec précédent ne doit rien laisser derrière lui.
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    EXPECT_TRUE(pa.close());
}

//--- Test Open : chemins de succès ------------------------------------------

TEST_F(PulseAudioBackendHwTest, TestOpenSucceed) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    auto ret = pa.open(makeOutputConfig());
    ASSERT_TRUE(ret);
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestOpenInputSucceed) {
    SKIP_UNLESS(input);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeInputConfig()));
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestOpenWithEmptyIdSucceeds) {
    // Id vide = sink/source par défaut du serveur.
    mka::audio::PulseAudio pa;
    auto cfg = validOutputConfig();
    cfg.id = "";
    ASSERT_TRUE(pa.open(cfg));
    ASSERT_TRUE(pa.start());
    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestOpenTwiceFailsWithInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));

    auto ret = pa.open(makeOutputConfig());
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(pa.close());
}

//--- Test Start ---------------------------------------------------------------

TEST_F(PulseAudioBackendHwTest, TestStartSucceedsAfterOpen) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));

    auto ret = pa.start();
    ASSERT_TRUE(ret);

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestStartTwiceFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());

    auto ret = pa.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestStartInvokesCallbackOnDifferentThread) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.setProcessFunction(testCallback));
    ASSERT_TRUE(pa.open(makeOutputConfig()));

    const auto callingThreadId = std::this_thread::get_id();
    ASSERT_TRUE(pa.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    ASSERT_NE(g_callbackThreadId.load(), callingThreadId);

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestStartWithoutCallbackDoesNotCrash) {
    // Chemin "silence par défaut" (aucun callback défini) : ne doit pas
    // planter, même si le contenu audio n'est pas vérifiable ici.
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestCallbackReceivesConfiguredOutputBuffers) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::PulseAudio pa;
    const auto cfg = makeOutputConfig();
    ASSERT_TRUE(pa.setProcessFunction(inspectCallback));
    ASSERT_TRUE(pa.open(cfg));
    ASSERT_TRUE(pa.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    EXPECT_EQ(g_inCount.load(), 0u);
    EXPECT_EQ(g_outCount.load(), cfg.outputChannels);
    // <= et non == : PulseAudio ne garantit pas des blocs de exactement
    // bufferSize frames (contrairement à JACK), seulement un maximum.
    EXPECT_GT(g_frames.load(), 0u);
    EXPECT_LE(g_frames.load(), cfg.bufferSize);
    EXPECT_TRUE(g_pointersValid.load());

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestCallbackReceivesConfiguredInputBuffers) {
    SKIP_UNLESS(input);

    resetCallbackState();
    mka::audio::PulseAudio pa;
    const auto cfg = makeInputConfig();
    ASSERT_TRUE(pa.setProcessFunction(inspectCallback));
    ASSERT_TRUE(pa.open(cfg));
    ASSERT_TRUE(pa.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }, 5000));
    EXPECT_EQ(g_outCount.load(), 0u);
    EXPECT_EQ(g_inCount.load(), cfg.inputChannels);
    EXPECT_GT(g_frames.load(), 0u);
    EXPECT_LE(g_frames.load(), cfg.bufferSize);
    EXPECT_TRUE(g_pointersValid.load());

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestSetProcessFunctionFailsWhileRunning) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());

    auto ret = pa.setProcessFunction(testCallback);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

//--- Test Stop ----------------------------------------------------------------

TEST_F(PulseAudioBackendHwTest, TestStopWithoutStartAfterOpenFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));

    auto ret = pa.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestStopSucceedsAfterStart) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());

    auto ret = pa.stop();
    ASSERT_TRUE(ret);

    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestStopTwiceFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());
    ASSERT_TRUE(pa.stop());

    auto ret = pa.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestStopActuallyHaltsCallbackInvocations) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.setProcessFunction(testCallback));
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    ASSERT_TRUE(pa.stop());

    g_callbackCalled.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_FALSE(g_callbackCalled.load());

    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestStopAllowsRestartAndCallbackResumes) {
    // Spécifique à PulseAudio : stop_ arrête réellement le thread
    // (threadStarted_ redevient false), donc start_ doit pouvoir le relancer
    // proprement -- pas seulement décorquer un flux existant.
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.setProcessFunction(testCallback));
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());
    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    ASSERT_TRUE(pa.stop());

    g_callbackCalled.store(false);
    ASSERT_TRUE(pa.start());
    EXPECT_TRUE(waitFor([] { return g_callbackCalled.load(); })) << "callback muet après restart";

    EXPECT_TRUE(pa.stop());
    EXPECT_TRUE(pa.close());
}

//--- Test Close -----------------------------------------------------------

TEST_F(PulseAudioBackendHwTest, TestCloseSucceedsAfterOpen) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));

    auto ret = pa.close();
    ASSERT_TRUE(ret);
}

TEST_F(PulseAudioBackendHwTest, TestCloseFailsWhileRunning) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());

    // Ordre imposé open -> start -> stop -> close.
    auto ret = pa.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(pa.stop());
    ASSERT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestCloseTwiceFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.close());

    auto ret = pa.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST_F(PulseAudioBackendHwTest, TestCloseReleasesResourcesForReopen) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.close());

    auto ret = pa.open(makeOutputConfig());
    ASSERT_TRUE(ret);
    EXPECT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestFullLifecycleOpenStartStopClose) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.open(makeOutputConfig()));
    ASSERT_TRUE(pa.start());
    ASSERT_TRUE(pa.stop());
    ASSERT_TRUE(pa.close());
}

TEST_F(PulseAudioBackendHwTest, TestLifecycleRepeatedCycles) {
    // Contexte et stream sont recréés à chaque start_ côté PulseAudio :
    // ce test cible spécifiquement d'éventuelles fuites ou incohérences
    // d'état sur des cycles répétés.
    SKIP_UNLESS(output);

    mka::audio::PulseAudio pa;
    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(i);
        ASSERT_TRUE(pa.open(makeOutputConfig()));
        ASSERT_TRUE(pa.start());
        ASSERT_TRUE(pa.stop());
        ASSERT_TRUE(pa.close());
    }
}

//--- Test destruction et instances multiples ---------------------------------

TEST_F(PulseAudioBackendHwTest, TestDestructorWhileOpenReleasesResources) {
    SKIP_UNLESS(output);

    {
        mka::audio::PulseAudio pa;
        ASSERT_TRUE(pa.open(makeOutputConfig()));
        // destruction sans close()
    }

    mka::audio::PulseAudio second;
    ASSERT_TRUE(second.open(makeOutputConfig()));
    EXPECT_TRUE(second.close());
}

TEST_F(PulseAudioBackendHwTest, TestDestructorWhileRunningReleasesResources) {
    SKIP_UNLESS(output);

    {
        mka::audio::PulseAudio pa;
        ASSERT_TRUE(pa.setProcessFunction(testCallback));
        ASSERT_TRUE(pa.open(makeOutputConfig()));
        ASSERT_TRUE(pa.start());
        // destruction sans stop() ni close()
    }

    mka::audio::PulseAudio second;
    ASSERT_TRUE(second.open(makeOutputConfig()));
    EXPECT_TRUE(second.close());
}

TEST_F(PulseAudioBackendHwTest, TestTwoInstancesCanCoexist) {
    SKIP_UNLESS(output);

    mka::audio::PulseAudio a;
    mka::audio::PulseAudio b;
    ASSERT_TRUE(a.setProcessFunction(testCallback));
    ASSERT_TRUE(b.setProcessFunction(testCallback));
    ASSERT_TRUE(a.open(makeOutputConfig()));
    ASSERT_TRUE(b.open(makeOutputConfig()));
    ASSERT_TRUE(a.start());
    ASSERT_TRUE(b.start());

    EXPECT_TRUE(b.stop());
    EXPECT_TRUE(a.stop());
    EXPECT_TRUE(b.close());
    EXPECT_TRUE(a.close());
}

//--- Test Sinewave (manuel, désactivé par défaut) ---------------------------

namespace {
    double g_sinePhase = 0.0;
    double g_sinePhaseIncrement = 0.0;
    float g_sineGain = 0.0f;
    float g_sineGainStep = 0.0f;
    std::atomic<bool> g_sineFadeOut{false};

    void sineCallback(const mka::audio::AudioProcessContext &ctx) noexcept {
        for (std::uint32_t i = 0; i < ctx.frames; ++i) {
            constexpr float amplitude = 0.2f;
            const float target = g_sineFadeOut.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
            g_sineGain += std::clamp(target - g_sineGain, -g_sineGainStep, g_sineGainStep);

            const auto sample = static_cast<float>(std::sin(g_sinePhase) * amplitude * g_sineGain);
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
TEST(PulseAudioBackendTest, TestPlaySineWave) {
    const auto out = discoverOutput();
    ASSERT_TRUE(out.available) << "aucun endpoint de sortie PulseAudio";
    std::println("output device: {}", out.id);

    constexpr double frequency = 440.0;

    mka::audio::PulseAudio pa;
    auto config = validOutputConfig(out.id);

    g_sinePhase = 0.0;
    g_sinePhaseIncrement = 2.0 * std::numbers::pi * frequency / static_cast<double>(config.sampleRate);
    g_sineGain = 0.0f;
    g_sineGainStep = static_cast<float>(1.0 / (0.02 * config.sampleRate)); // rampe de 20 ms
    g_sineFadeOut.store(false);

    ASSERT_TRUE(pa.setProcessFunction(sineCallback));
    ASSERT_TRUE(pa.open(config));
    ASSERT_TRUE(pa.start());

    std::this_thread::sleep_for(std::chrono::seconds(3));

    g_sineFadeOut.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_TRUE(pa.stop());
    ASSERT_TRUE(pa.close());
}

//--- Test Record & Playback (manuel, désactivé par défaut) ------------------
//
// Mêmes précautions anti-click que les versions PipeWire/JACK : warm-up de
// capture, fades hors temps réel, silence avant/après le signal à la lecture.

namespace {
    std::vector<std::vector<float>> g_recordedSamples;
    std::atomic<std::size_t> g_recordedFrames{0};
    std::atomic<std::size_t> g_warmupFramesLeft{0};
    std::size_t g_recordCapacityFrames = 0;

    void recordCallback(const mka::audio::AudioProcessContext &ctx) noexcept {
        std::size_t start = 0;
        const std::size_t warm = g_warmupFramesLeft.load();
        if (warm > 0) {
            start = std::min<std::size_t>(warm, ctx.frames);
            g_warmupFramesLeft.store(warm - start);
        }

        const std::size_t available = ctx.frames - start;
        const std::size_t writeOffset = g_recordedFrames.load();
        const std::size_t framesToCopy = std::min<std::size_t>(g_recordCapacityFrames - writeOffset, available);

        for (std::uint32_t ch = 0; ch < ctx.input.count && ch < g_recordedSamples.size(); ++ch) {
            for (std::size_t i = 0; i < framesToCopy; ++i) {
                g_recordedSamples[ch][writeOffset + i] = ctx.input.channels[ch][start + i];
            }
        }

        g_recordedFrames.fetch_add(framesToCopy);
    }

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

    void applyFades(std::vector<std::vector<float>> &samples, const std::size_t fadeFrames) {
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

// Désactivé par défaut : enregistre depuis un endpoint source et rejoue sur
// un endpoint destination. À activer manuellement.
TEST(PulseAudioBackendTest, TestRecordAndPlayback) {
    const auto in = discoverInput();
    const auto out = discoverOutput();
    ASSERT_TRUE(in.available) << "aucun endpoint d'entrée PulseAudio";
    ASSERT_TRUE(out.available) << "aucun endpoint de sortie PulseAudio";
    std::println("input device:  {}", in.id);
    std::println("output device: {}", out.id);

    constexpr std::uint32_t channels = 2;
    constexpr int recordSeconds = 5;

    const auto msToFrames = [&](const int ms) {
        return static_cast<std::size_t>(kSampleRate) * ms / 1000;
    };
    constexpr std::size_t warmupFrames = msToFrames(300);
    constexpr std::size_t fadeFrames = msToFrames(20);
    constexpr std::size_t silenceFrames = msToFrames(300);

    g_recordCapacityFrames = static_cast<std::size_t>(kSampleRate) * recordSeconds;
    g_recordedSamples.assign(channels, std::vector<float>(g_recordCapacityFrames, 0.0f));
    g_recordedFrames.store(0);
    g_warmupFramesLeft.store(warmupFrames);

    // --- Phase 1 : enregistrement ---
    {
        mka::audio::PulseAudio recorder;
        const mka::audio::EndpointConfig recordConfig {
            .id = in.id,
            .direction = mka::audio::Direction::Input,
            .inputChannels = channels,
            .outputChannels = 0,
            .sampleRate = kSampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = kBufferSize,
        };

        ASSERT_TRUE(recorder.setProcessFunction(recordCallback));
        ASSERT_TRUE(recorder.open(recordConfig));
        ASSERT_TRUE(recorder.start());

        ASSERT_TRUE(waitFor([] { return g_recordedFrames.load() >= g_recordCapacityFrames; },
                            (recordSeconds + 5) * 1000))
            << "l'enregistrement n'a pas abouti (aucune donnée reçue ?)";

        ASSERT_TRUE(recorder.stop());
        ASSERT_TRUE(recorder.close());
    }

    applyFades(g_recordedSamples, fadeFrames);

    std::this_thread::sleep_for(std::chrono::seconds(3));

    // --- Phase 2 : lecture ---
    g_playbackLeadFrames = silenceFrames;
    g_playbackTotalFrames = silenceFrames + g_recordCapacityFrames + silenceFrames;
    g_playbackPosition.store(0);

    {
        mka::audio::PulseAudio player;
        const mka::audio::EndpointConfig playbackConfig {
            .id = out.id,
            .direction = mka::audio::Direction::Output,
            .inputChannels = 0,
            .outputChannels = channels,
            .sampleRate = kSampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = kBufferSize,
        };

        ASSERT_TRUE(player.setProcessFunction(playbackFromRecordCallback));
        ASSERT_TRUE(player.open(playbackConfig));
        ASSERT_TRUE(player.start());

        ASSERT_TRUE(waitFor([] { return g_playbackPosition.load() >= g_playbackTotalFrames; },
                            static_cast<int>((g_playbackTotalFrames / kSampleRate + 5) * 1000)));

        ASSERT_TRUE(player.stop());
        ASSERT_TRUE(player.close());
    }
}
