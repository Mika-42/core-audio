//
// Tests du backend JACK (mka.audio.jack)
//
// Organisation :
//  - JackBackendTest      : tests sans dépendance à un serveur JACK (garde-fous
//                           locaux, états). Ils passent avec ou sans serveur.
//  - JackBackendHwTest    : tests nécessitant un serveur JACK actif (jackd ou
//                           pipewire-jack). Les endpoints sont découverts au
//                           lancement de la suite ; chaque test s'ignore
//                           (GTEST_SKIP) si l'endpoint dont il a besoin manque.
//
// Tous les callbacks de test écrivent du silence sur leurs sorties : les
// buffers de sortie JACK ne sont pas garantis nuls, et ces tests sont
// connectés à de vrais ports (ex. system:playback_N).
//
#include <gtest/gtest.h>
#include <jack/jack.h>
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

import mka.audio.backend.jack;
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

    void testCallback(void*, const mka::audio::AudioProcessContext& ctx) noexcept {
        g_callbackThreadId.store(std::this_thread::get_id());
        g_callbackCalled.store(true);
        silence(ctx);
    }

    void inspectCallback(void*, const mka::audio::AudioProcessContext& ctx) noexcept {
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

    template <class Pred>
    bool waitFor(Pred pred, const int timeoutMs = 2000) {
        for (int waited = 0; waited < timeoutMs; waited += 20) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    }

    bool startsWith(const std::string_view s, const std::string_view prefix) {
        return s.starts_with(prefix);
    }

    int connectionsToMka(const std::string& endpointId) {
        jack_status_t status{};
        jack_client_t* probe = jack_client_open("mka-test-probe", JackNoStartServer, &status);
        if (!probe) return -1;

        int count = 0;
        const char** ports = jack_get_ports(probe, nullptr, JACK_DEFAULT_AUDIO_TYPE, 0);
        if (ports) {
            const std::string prefix = endpointId + ":";
            for (const char** p = ports; *p; ++p) {
                if (!startsWith(*p, prefix)) continue;
                const jack_port_t* port = jack_port_by_name(probe, *p);
                if (!port) continue;
                const char** conns = jack_port_get_all_connections(probe, port);
                if (!conns) continue;
                for (const char** c = conns; *c; ++c) {
                    if (startsWith(*c, "mka-audio")) ++count;
                }
                jack_free(conns);
            }
            jack_free(ports);
        }
        jack_client_close(probe);
        return count;
    }

    mka::audio::EndpointConfig validOutputConfig(std::string id = "") {
        return mka::audio::EndpointConfig{
            .id = std::move(id),
            .direction = mka::audio::Direction::Output,
            .inputChannels = 0,
            .outputChannels = 2,
            .sampleRate = 48000,
            .format = mka::audio::Format::Float32,
            .bufferSize = 512,
        };
    }

    void expectOpenError(mka::audio::JACK& jack, const mka::audio::EndpointConfig& cfg, const mka::audio::ErrorType expected) {
        const auto ret = jack.open(cfg);
        ASSERT_FALSE(ret) << "open() aurait dû échouer";
        EXPECT_EQ(ret.error(), expected);
    }
}

//--- Test GetEndPoints ------------------------------------------------------

TEST(JackBackendTest, TestGetEndPoints) {
    const mka::audio::JACK jack;

    for (const auto endpoints = jack.getEndPoints(); const auto& endpoint : endpoints) {
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

TEST(JackBackendTest, TestGetEndPointsDoesNotThrow) {
    const mka::audio::JACK jack;
    ASSERT_NO_THROW({
        auto endpoints = jack.getEndPoints();
    });
}

TEST(JackBackendTest, TestGetEndPointsCallableWhileClosed) {
    const mka::audio::JACK jack;
    ASSERT_NO_THROW({
        auto endpoints = jack.getEndPoints();
        (void) endpoints;
    });
}

TEST(JackBackendTest, TestGetEndPointsIdsAreUnique) {
    // Les ports sont regroupés par client : un client = un seul endpoint.
    const mka::audio::JACK jack;
    const auto endpoints = jack.getEndPoints();

    std::vector<std::string> ids;
    for (const auto& e : endpoints) ids.push_back(e.id);
    std::ranges::sort(ids);

    EXPECT_EQ(std::ranges::adjacent_find(ids), ids.end()) << "id d'endpoint en double";
}

TEST(JackBackendTest, TestGetEndPointsCapabilitiesReflectServer) {
    const mka::audio::JACK jack;
    const auto endpoints = jack.getEndPoints();
    if (endpoints.empty()) {
        GTEST_SKIP() << "aucun endpoint (serveur JACK absent ?)";
    }

    std::optional<mka::audio::SampleRate> rate;
    std::optional<mka::audio::BufferSize> bufferSize;

    const auto check = [&](const mka::audio::StreamCapabilities& caps) {
        EXPECT_EQ(caps.minChannels, 1u);
        EXPECT_GE(caps.maxChannels, 1u);


        ASSERT_EQ(caps.sampleRates.size(), 1u);
        ASSERT_EQ(caps.bufferSizes.size(), 1u);
        ASSERT_EQ(caps.formats.size(), 1u);
        EXPECT_TRUE(caps.formats.front() == mka::audio::Format::Float32);

        if (!rate) rate = caps.sampleRates.front();
        if (!bufferSize) bufferSize = caps.bufferSizes.front();
        EXPECT_EQ(caps.sampleRates.front(), *rate) << "rate différent entre endpoints";
        EXPECT_EQ(caps.bufferSizes.front(), *bufferSize) << "buffer différent entre endpoints";
    };

    for (const auto& endpoint : endpoints) {
        if (endpoint.input) check(*endpoint.input);
        if (endpoint.output) check(*endpoint.output);
    }
}

TEST(JackBackendTest, TestGetEndPointsContainsExpectedDeviceIfConfigured) {
    const char* expectedId = std::getenv("MKA_TEST_JACK_DEVICE_ID");
    if (!expectedId || std::string_view{expectedId}.empty()) {
        GTEST_SKIP() << "MKA_TEST_JACK_DEVICE_ID non défini, test ignoré";
    }

    const mka::audio::JACK jack;
    const auto endpoints = jack.getEndPoints();

    const auto it = std::ranges::find_if(endpoints, [&](const mka::audio::Endpoint& e) {
        return e.id == expectedId;
    });
    ASSERT_NE(it, endpoints.end()) << "endpoint attendu introuvable: " << expectedId;
}

//--- Test Open : garde-fous indépendants du serveur -------------------------

TEST(JackBackendTest, TestOpenInvalidSampleRateRejected) {
    mka::audio::JACK jack;
    auto cfg = validOutputConfig();
    cfg.sampleRate = 44190; // n'appartient pas à supportedSampleRates
    expectOpenError(jack, cfg, mka::audio::ErrorType::SampleRateNotSupported);
}

TEST(JackBackendTest, TestOpenInvalidBufferSizeRejected) {
    mka::audio::JACK jack;
    auto cfg = validOutputConfig();
    cfg.bufferSize = 500; // n'appartient pas à supportedBufferSizes
    expectOpenError(jack, cfg, mka::audio::ErrorType::BufferSizeNotSupported);
}

TEST(JackBackendTest, TestOpenInvalidFormatRejected) {

    mka::audio::JACK jack;
    for (const auto format : mka::audio::supportedFormats) {
        if (format == mka::audio::Format::Float32) continue;
        SCOPED_TRACE(fmtToStr(format));

        auto cfg = validOutputConfig();
        cfg.format = format;
        expectOpenError(jack, cfg, mka::audio::ErrorType::FormatNotSupported);
    }
}

TEST(JackBackendTest, TestOpenInputZeroChannelsRejected) {
    mka::audio::JACK jack;
    auto cfg = validOutputConfig();
    cfg.direction = mka::audio::Direction::Input;
    cfg.inputChannels = 0;
    cfg.outputChannels = 2; // ignoré pour une direction Input
    expectOpenError(jack, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(JackBackendTest, TestOpenOutputZeroChannelsRejected) {
    mka::audio::JACK jack;
    auto cfg = validOutputConfig();
    cfg.inputChannels = 2; // ignoré pour une direction Output
    cfg.outputChannels = 0;
    expectOpenError(jack, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(JackBackendTest, TestOpenDuplexZeroInputChannelsRejected) {
    mka::audio::JACK jack;
    auto cfg = validOutputConfig();
    cfg.direction = mka::audio::Direction::Duplex;
    cfg.inputChannels = 0;
    cfg.outputChannels = 2;
    expectOpenError(jack, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(JackBackendTest, TestOpenDuplexZeroOutputChannelsRejected) {
    mka::audio::JACK jack;
    auto cfg = validOutputConfig();
    cfg.direction = mka::audio::Direction::Duplex;
    cfg.inputChannels = 2;
    cfg.outputChannels = 0;
    expectOpenError(jack, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(JackBackendTest, TestOpenValidationErrorPrecedence) {

    mka::audio::JACK jack;
    mka::audio::EndpointConfig cfg{
        .id = "",
        .direction = mka::audio::Direction::Output,
        .inputChannels = 0,
        .outputChannels = 0,
        .sampleRate = 44190,
        .format = mka::audio::Format::Int16,
        .bufferSize = 500,
    };

    expectOpenError(jack, cfg, mka::audio::ErrorType::SampleRateNotSupported);
    cfg.sampleRate = 48000;
    expectOpenError(jack, cfg, mka::audio::ErrorType::BufferSizeNotSupported);
    cfg.bufferSize = 512;
    expectOpenError(jack, cfg, mka::audio::ErrorType::FormatNotSupported);
    cfg.format = mka::audio::Format::Float32;
    expectOpenError(jack, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST(JackBackendTest, TestOpenInvalidIDFails) {
    mka::audio::JACK jack;
    expectOpenError(jack, validOutputConfig("this-client-does-not-exist-999999"), mka::audio::ErrorType::EndpointUnavailable);
}

//--- Test Start / Stop / Close : garde-fous d'état --------------------------

TEST(JackBackendTest, TestStartWithoutOpenFailsInvalidState) {
    mka::audio::JACK jack;

    auto ret = jack.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(JackBackendTest, TestStopWithoutStartFailsInvalidState) {
    mka::audio::JACK jack;

    auto ret = jack.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(JackBackendTest, TestCloseWithoutOpenFailsInvalidState) {
    mka::audio::JACK jack;

    auto ret = jack.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST(JackBackendTest, TestSetProcessFunctionAllowedWhenClosed) {
    mka::audio::JACK jack;

    auto ret = jack.setProcessFunction(testCallback);
    ASSERT_TRUE(ret);
}

TEST(JackBackendTest, TestFailedOpenDoesNotChangeState) {
    // Après un open échoué, l'état reste Closed : start/stop/close refusés.
    mka::audio::JACK jack;
    auto cfg = validOutputConfig();
    cfg.format = mka::audio::Format::Int16;
    expectOpenError(jack, cfg, mka::audio::ErrorType::FormatNotSupported);

    auto start = jack.start();
    ASSERT_FALSE(start);
    EXPECT_EQ(start.error(), mka::audio::ErrorType::InvalidState);

    auto close = jack.close();
    ASSERT_FALSE(close);
    EXPECT_EQ(close.error(), mka::audio::ErrorType::InvalidState);
}

//--- Fixture pour les tests nécessitant un serveur JACK ---------------------

namespace {
    struct DiscoveredEndpoint {
        bool available = false;
        std::string id;
        mka::audio::SampleRate sampleRate = 0;
        mka::audio::BufferSize bufferSize = 0;
        std::uint32_t inMax = 0;
        std::uint32_t outMax = 0;
    };

    DiscoveredEndpoint discoverEndpoint(const bool needInput, const bool needOutput) {
        const mka::audio::JACK jack;
        for (const auto& e : jack.getEndPoints()) {
            if (needInput && !e.input) continue;
            if (needOutput && !e.output) continue;

            const auto& caps = needOutput ? *e.output : *e.input;
            if (caps.sampleRates.empty() || caps.bufferSizes.empty()) continue;

            const auto rate = caps.sampleRates.front();
            const auto size = caps.bufferSizes.front();
            if (std::ranges::find(mka::audio::supportedSampleRates, rate) == mka::audio::supportedSampleRates.end()) continue;
            if (std::ranges::find(mka::audio::supportedBufferSizes, size) == mka::audio::supportedBufferSizes.end()) continue;

            return DiscoveredEndpoint{
                .available = true,
                .id = e.id,
                .sampleRate = rate,
                .bufferSize = size,
                .inMax = e.input ? e.input->maxChannels : 0,
                .outMax = e.output ? e.output->maxChannels : 0,
            };
        }
        return {};
    }
}

#define SKIP_UNLESS(ep) \
    do { \
        if (!(ep).available) { \
            GTEST_SKIP() << "endpoint '" #ep "' indisponible (serveur JACK absent, ou rate/buffer hors valeurs supportées)"; \
        } \
    } while (0)

class JackBackendHwTest : public ::testing::Test {
    protected:
        static DiscoveredEndpoint output;
        static DiscoveredEndpoint input;
        static DiscoveredEndpoint duplex;

        static void SetUpTestSuite() {
            output = discoverEndpoint(false, true);
            input = discoverEndpoint(true, false);
            duplex = discoverEndpoint(true, true);
        }

        static mka::audio::EndpointConfig makeOutputConfig() {
            return mka::audio::EndpointConfig{
                .id = output.id,
                .direction = mka::audio::Direction::Output,
                .inputChannels = 0,
                .outputChannels = std::min<std::uint32_t>(2, output.outMax),
                .sampleRate = output.sampleRate,
                .format = mka::audio::Format::Float32,
                .bufferSize = output.bufferSize,
            };
        }

        static mka::audio::EndpointConfig makeInputConfig() {
            return mka::audio::EndpointConfig{
                .id = input.id,
                .direction = mka::audio::Direction::Input,
                .inputChannels = std::min<std::uint32_t>(2, input.inMax),
                .outputChannels = 0,
                .sampleRate = input.sampleRate,
                .format = mka::audio::Format::Float32,
                .bufferSize = input.bufferSize,
            };
        }

        static mka::audio::EndpointConfig makeDuplexConfig() {
            return mka::audio::EndpointConfig{
                .id = duplex.id,
                .direction = mka::audio::Direction::Duplex,
                .inputChannels = std::min<std::uint32_t>(2, duplex.inMax),
                .outputChannels = std::min<std::uint32_t>(2, duplex.outMax),
                .sampleRate = duplex.sampleRate,
                .format = mka::audio::Format::Float32,
                .bufferSize = duplex.bufferSize,
            };
        }
};
DiscoveredEndpoint JackBackendHwTest::output;
DiscoveredEndpoint JackBackendHwTest::input;
DiscoveredEndpoint JackBackendHwTest::duplex;

//--- Test GetEndPoints avec serveur -----------------------------------------

TEST_F(JackBackendHwTest, TestGetEndPointsSeesOwnClientWhileRunning) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(testCallback));
    ASSERT_TRUE(jack.open(makeOutputConfig()));

    ASSERT_TRUE(jack.start());

    const bool seen = waitFor([&] {
        const auto endpoints = jack.getEndPoints();
        return std::ranges::any_of(endpoints, [](const mka::audio::Endpoint& e) {
            return startsWith(e.id, "mka-audio") && e.input.has_value();
        });
    });
    EXPECT_TRUE(seen) << "le client mka-audio n'apparaît pas dans getEndPoints()";

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestGetEndPointsCallableWhileRunning) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(testCallback));
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());

    ASSERT_NO_THROW({
        const auto endpoints = jack.getEndPoints();
        EXPECT_FALSE(endpoints.empty());
    });

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

//--- Test Open : erreurs dépendant du serveur ------------------------------

TEST_F(JackBackendHwTest, TestOpenServerSampleRateMismatchRejected) {
    SKIP_UNLESS(output);

    const auto other = std::ranges::find_if(mka::audio::supportedSampleRates, [&](const auto rate) {
        return rate != output.sampleRate;
    });
    ASSERT_NE(other, mka::audio::supportedSampleRates.end());

    mka::audio::JACK jack;
    auto cfg = makeOutputConfig();
    cfg.sampleRate = *other;
    expectOpenError(jack, cfg, mka::audio::ErrorType::SampleRateNotSupported);
}

TEST_F(JackBackendHwTest, TestOpenServerBufferSizeMismatchRejected) {
    SKIP_UNLESS(output);

    const auto other = std::ranges::find_if(mka::audio::supportedBufferSizes, [&](const auto size) {
        return size != output.bufferSize;
    });
    ASSERT_NE(other, mka::audio::supportedBufferSizes.end());

    mka::audio::JACK jack;
    auto cfg = makeOutputConfig();
    cfg.bufferSize = *other;
    expectOpenError(jack, cfg, mka::audio::ErrorType::BufferSizeNotSupported);
}

TEST_F(JackBackendHwTest, TestOpenTooManyOutputChannelsRejected) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    auto cfg = makeOutputConfig();
    cfg.outputChannels = output.outMax + 1; // plus que de ports disponibles
    expectOpenError(jack, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST_F(JackBackendHwTest, TestOpenTooManyInputChannelsRejected) {
    SKIP_UNLESS(input);

    mka::audio::JACK jack;
    auto cfg = makeInputConfig();
    cfg.inputChannels = input.inMax + 1;
    expectOpenError(jack, cfg, mka::audio::ErrorType::ChannelsNotSupported);
}

TEST_F(JackBackendHwTest, TestOpenInputOnlyEndpointAsOutputFails) {

    SKIP_UNLESS(output);

    const mka::audio::JACK discoverer;
    const auto endpoints = discoverer.getEndPoints();
    const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.input.has_value() && !e.output.has_value();
    });
    if (it == endpoints.end()) GTEST_SKIP() << "aucun endpoint source-only trouvé";

    mka::audio::JACK jack;
    expectOpenError(jack, validOutputConfig(it->id), mka::audio::ErrorType::EndpointUnavailable);
}

TEST_F(JackBackendHwTest, TestOpenOutputOnlyEndpointAsInputFails) {
    SKIP_UNLESS(output);

    const mka::audio::JACK discoverer;
    const auto endpoints = discoverer.getEndPoints();
    const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.output.has_value() && !e.input.has_value();
    });
    if (it == endpoints.end()) GTEST_SKIP() << "aucun endpoint destination-only trouvé";

    mka::audio::JACK jack;
    auto cfg = validOutputConfig(it->id);
    cfg.direction = mka::audio::Direction::Input;
    cfg.inputChannels = 2;
    cfg.outputChannels = 0;
    expectOpenError(jack, cfg, mka::audio::ErrorType::EndpointUnavailable);
}

TEST_F(JackBackendHwTest, TestOpenDuplexOnOneSidedEndpointFails) {

    SKIP_UNLESS(output);

    const mka::audio::JACK discoverer;
    const auto endpoints = discoverer.getEndPoints();
    const auto it = std::ranges::find_if(endpoints, [](const mka::audio::Endpoint& e) {
        return e.input.has_value() != e.output.has_value();
    });
    if (it == endpoints.end()) GTEST_SKIP() << "aucun endpoint à sens unique trouvé";

    mka::audio::JACK jack;
    auto cfg = validOutputConfig(it->id);
    cfg.direction = mka::audio::Direction::Duplex;
    cfg.inputChannels = 1;
    cfg.outputChannels = 1;
    expectOpenError(jack, cfg, mka::audio::ErrorType::EndpointUnavailable);
}

TEST_F(JackBackendHwTest, TestOpenFailureLeavesStateClosedAllowingRetry) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;

    auto badId = makeOutputConfig();
    badId.id = "still-not-a-real-client";
    expectOpenError(jack, badId, mka::audio::ErrorType::EndpointUnavailable);

    auto tooMany = makeOutputConfig();
    tooMany.outputChannels = output.outMax + 1;
    expectOpenError(jack, tooMany, mka::audio::ErrorType::ChannelsNotSupported);

    ASSERT_TRUE(jack.open(makeOutputConfig()));
    EXPECT_TRUE(jack.close());
}

//--- Test Open : chemins de succès ------------------------------------------

TEST_F(JackBackendHwTest, TestOpenSucceed) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    auto ret = jack.open(makeOutputConfig());
    ASSERT_TRUE(ret);
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestOpenInputSucceed) {
    SKIP_UNLESS(input);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeInputConfig()));
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestOpenDuplexSucceed) {
    SKIP_UNLESS(duplex);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeDuplexConfig()));
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestOpenWithEmptyIdSucceeds) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    auto cfg = makeOutputConfig();
    cfg.id = "";
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());
    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestOpenOutputIgnoresInputChannelCount) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::JACK jack;
    auto cfg = makeOutputConfig();
    cfg.inputChannels = 99; // ignoré pour une direction Output
    ASSERT_TRUE(jack.setProcessFunction(inspectCallback));
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    EXPECT_EQ(g_inCount.load(), 0u);
    EXPECT_EQ(g_outCount.load(), cfg.outputChannels);

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestOpenInputIgnoresOutputChannelCount) {
    SKIP_UNLESS(input);

    resetCallbackState();
    mka::audio::JACK jack;
    auto cfg = makeInputConfig();
    cfg.outputChannels = 99; // ignoré pour une direction Input
    ASSERT_TRUE(jack.setProcessFunction(inspectCallback));
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    EXPECT_EQ(g_outCount.load(), 0u);
    EXPECT_EQ(g_inCount.load(), cfg.inputChannels);

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestOpenTwiceFailsWithInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));

    auto ret = jack.open(makeOutputConfig());
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(jack.close());
}

//--- Test Start ---------------------------------------------------------------

TEST_F(JackBackendHwTest, TestStartSucceedsAfterOpen) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));

    auto ret = jack.start();
    ASSERT_TRUE(ret);

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStartTwiceFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());

    auto ret = jack.start();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStartInvokesCallbackOnDifferentThread) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(testCallback));
    ASSERT_TRUE(jack.open(makeOutputConfig()));

    const auto callingThreadId = std::this_thread::get_id();
    ASSERT_TRUE(jack.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    ASSERT_NE(g_callbackThreadId.load(), callingThreadId);

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestCallbackReceivesConfiguredOutputBuffers) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::JACK jack;
    const auto cfg = makeOutputConfig();
    ASSERT_TRUE(jack.setProcessFunction(inspectCallback));
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    EXPECT_EQ(g_inCount.load(), 0u);
    EXPECT_EQ(g_outCount.load(), cfg.outputChannels);
    EXPECT_EQ(g_frames.load(), cfg.bufferSize);
    EXPECT_TRUE(g_pointersValid.load());

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestCallbackReceivesConfiguredInputBuffers) {
    SKIP_UNLESS(input);

    resetCallbackState();
    mka::audio::JACK jack;
    const auto cfg = makeInputConfig();
    ASSERT_TRUE(jack.setProcessFunction(inspectCallback));
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    EXPECT_EQ(g_outCount.load(), 0u);
    EXPECT_EQ(g_inCount.load(), cfg.inputChannels);
    EXPECT_EQ(g_frames.load(), cfg.bufferSize);
    EXPECT_TRUE(g_pointersValid.load());

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestSetProcessFunctionFailsWhileRunning) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());

    auto ret = jack.setProcessFunction(testCallback);
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStartConnectsEndpointPorts) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    const auto cfg = makeOutputConfig();
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());

    EXPECT_TRUE(waitFor([&] {
        return connectionsToMka(output.id) >= static_cast<int>(cfg.outputChannels);
    })) << "les ports de l'endpoint ne sont pas connectés après start()";

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStartConnectsInputEndpointPorts) {
    SKIP_UNLESS(input);

    mka::audio::JACK jack;
    const auto cfg = makeInputConfig();
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());

    EXPECT_TRUE(waitFor([&] {
        return connectionsToMka(input.id) >= static_cast<int>(cfg.inputChannels);
    }));

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

//--- Test Stop ----------------------------------------------------------------

TEST_F(JackBackendHwTest, TestStopWithoutStartAfterOpenFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));

    auto ret = jack.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStopSucceedsAfterStart) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());

    auto ret = jack.stop();
    ASSERT_TRUE(ret);

    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStopTwiceFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());
    ASSERT_TRUE(jack.stop());

    auto ret = jack.stop();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStopActuallyHaltsCallbackInvocations) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(testCallback));
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());

    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    ASSERT_TRUE(jack.stop());

    g_callbackCalled.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_FALSE(g_callbackCalled.load());

    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStopDisconnectsEndpointPorts) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    const auto cfg = makeOutputConfig();
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());
    ASSERT_TRUE(waitFor([&] { return connectionsToMka(output.id) > 0; }));

    ASSERT_TRUE(jack.stop());
    EXPECT_TRUE(waitFor([&] { return connectionsToMka(output.id) == 0; }))
        << "des connexions subsistent après stop()";

    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestStopAllowsRestartAndCallbackResumes) {
    SKIP_UNLESS(output);

    resetCallbackState();
    mka::audio::JACK jack;
    const auto cfg = makeOutputConfig();
    ASSERT_TRUE(jack.setProcessFunction(testCallback));
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());
    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    ASSERT_TRUE(jack.stop());

    g_callbackCalled.store(false);
    ASSERT_TRUE(jack.start());
    EXPECT_TRUE(waitFor([] { return g_callbackCalled.load(); })) << "callback muet après restart";
    EXPECT_TRUE(waitFor([&] {
        return connectionsToMka(output.id) >= static_cast<int>(cfg.outputChannels);
    })) << "connexions non refaites après restart";

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

//--- Test Close -----------------------------------------------------------

TEST_F(JackBackendHwTest, TestCloseSucceedsAfterOpen) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));

    auto ret = jack.close();
    ASSERT_TRUE(ret);
}

TEST_F(JackBackendHwTest, TestCloseFailsWhileRunning) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());

    auto ret = jack.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestCloseTwiceFailsInvalidState) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.close());

    auto ret = jack.close();
    ASSERT_FALSE(ret);
    ASSERT_EQ(ret.error(), mka::audio::ErrorType::InvalidState);
}

TEST_F(JackBackendHwTest, TestCloseReleasesResourcesForReopen) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.close());

    auto ret = jack.open(makeOutputConfig());
    ASSERT_TRUE(ret);
    EXPECT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestFullLifecycleOpenStartStopClose) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeOutputConfig()));
    ASSERT_TRUE(jack.start());
    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestLifecycleRepeatedCycles) {
    SKIP_UNLESS(output);

    mka::audio::JACK jack;
    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(i);
        ASSERT_TRUE(jack.open(makeOutputConfig()));
        ASSERT_TRUE(jack.start());
        ASSERT_TRUE(jack.stop());
        ASSERT_TRUE(jack.close());
    }
}

//--- Test Duplex --------------------------------------------------------------

TEST_F(JackBackendHwTest, TestDuplexFullLifecycle) {
    SKIP_UNLESS(duplex);

    mka::audio::JACK jack;
    ASSERT_TRUE(jack.open(makeDuplexConfig()));
    ASSERT_TRUE(jack.start());
    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());
}

TEST_F(JackBackendHwTest, TestDuplexCallbackReceivesInputAndOutput) {
    SKIP_UNLESS(duplex);

    resetCallbackState();
    mka::audio::JACK jack;
    const auto cfg = makeDuplexConfig();
    ASSERT_TRUE(jack.setProcessFunction(inspectCallback));
    ASSERT_TRUE(jack.open(cfg));
    ASSERT_TRUE(jack.start());

    // Un seul callback reçoit entrées et sorties ensemble.
    ASSERT_TRUE(waitFor([] { return g_callbackCalled.load(); }));
    EXPECT_EQ(g_inCount.load(), cfg.inputChannels);
    EXPECT_EQ(g_outCount.load(), cfg.outputChannels);
    EXPECT_EQ(g_frames.load(), cfg.bufferSize);
    EXPECT_TRUE(g_pointersValid.load());

    EXPECT_TRUE(waitFor([&] {
        return connectionsToMka(duplex.id) >= static_cast<int>(cfg.inputChannels + cfg.outputChannels);
    })) << "les ports input et output ne sont pas tous connectés";

    EXPECT_TRUE(jack.stop());
    EXPECT_TRUE(jack.close());
}

//--- Test destruction et instances multiples ---------------------------------

TEST_F(JackBackendHwTest, TestDestructorWhileOpenReleasesResources) {
    SKIP_UNLESS(output);

    {
        mka::audio::JACK jack;
        ASSERT_TRUE(jack.open(makeOutputConfig()));
        // destruction sans close()
    }

    mka::audio::JACK second;
    ASSERT_TRUE(second.open(makeOutputConfig()));
    EXPECT_TRUE(second.close());
}

TEST_F(JackBackendHwTest, TestDestructorWhileRunningReleasesResources) {
    SKIP_UNLESS(output);

    {
        mka::audio::JACK jack;
        ASSERT_TRUE(jack.setProcessFunction(testCallback));
        ASSERT_TRUE(jack.open(makeOutputConfig()));
        ASSERT_TRUE(jack.start());
    }

    mka::audio::JACK second;
    ASSERT_TRUE(second.open(makeOutputConfig()));
    EXPECT_TRUE(second.close());
}

TEST_F(JackBackendHwTest, TestTwoInstancesCanCoexist) {
    SKIP_UNLESS(output);

    mka::audio::JACK a;
    mka::audio::JACK b;
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

    void sineCallback(void*, const mka::audio::AudioProcessContext &ctx) noexcept {
        for (std::uint32_t i = 0; i < ctx.frames; ++i) {
            constexpr float amplitude = 0.2f;
            // Rampe de gain (fade-in au démarrage, fade-out avant l'arrêt) :
            // évite les clicks aux bords du signal.
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

TEST(JackBackendTest, DISABLED_TestPlaySineWave) {
    const auto out = discoverEndpoint(false, true);
    ASSERT_TRUE(out.available) << "aucun endpoint de sortie JACK";
    std::println("output device: {}", out.id);

    constexpr double frequency = 440.0;
    const double sampleRate = out.sampleRate;

    mka::audio::JACK jack;
    const mka::audio::EndpointConfig config {
        .id = out.id,
        .direction = mka::audio::Direction::Output,
        .inputChannels = 0,
        .outputChannels = std::min<std::uint32_t>(2, out.outMax),
        .sampleRate = out.sampleRate,
        .format = mka::audio::Format::Float32,
        .bufferSize = out.bufferSize,
    };

    g_sinePhase = 0.0;
    g_sinePhaseIncrement = 2.0 * std::numbers::pi * frequency / sampleRate;
    g_sineGain = 0.0f;
    g_sineGainStep = static_cast<float>(1.0 / (0.02 * sampleRate)); // rampe de 20 ms
    g_sineFadeOut.store(false);

    ASSERT_TRUE(jack.setProcessFunction(sineCallback));
    ASSERT_TRUE(jack.open(config));
    ASSERT_TRUE(jack.start());

    std::this_thread::sleep_for(std::chrono::seconds(3));

    g_sineFadeOut.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // laisse finir le fade-out

    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());
}

//--- Test Record & Playback (manuel, désactivé par défaut) ------------------

namespace {
    std::vector<std::vector<float>> g_recordedSamples;
    std::atomic<std::size_t> g_recordedFrames{0};
    std::atomic<std::size_t> g_warmupFramesLeft{0};
    std::size_t g_recordCapacityFrames = 0;

    void recordCallback(void*, const mka::audio::AudioProcessContext &ctx) noexcept {
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

    // Timeline de lecture : [ silence | signal | silence ]
    std::atomic<std::size_t> g_playbackPosition{0};
    std::size_t g_playbackLeadFrames = 0;
    std::size_t g_playbackTotalFrames = 0;

    void playbackFromRecordCallback(void*, const mka::audio::AudioProcessContext &ctx) noexcept {
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

TEST(JackBackendTest, DISABLED_TestRecordAndPlayback) {
    const auto in = discoverEndpoint(true, false);
    const auto out = discoverEndpoint(false, true);
    ASSERT_TRUE(in.available) << "aucun endpoint d'entrée JACK";
    ASSERT_TRUE(out.available) << "aucun endpoint de sortie JACK";
    ASSERT_EQ(in.sampleRate, out.sampleRate);
    std::println("input device:  {}", in.id);
    std::println("output device: {}", out.id);

    const std::uint32_t inChannels = std::min<std::uint32_t>(2, in.inMax);
    const std::uint32_t outChannels = std::min<std::uint32_t>(2, out.outMax);
    constexpr int recordSeconds = 5;
    const mka::audio::SampleRate sampleRate = in.sampleRate;

    const auto msToFrames = [&](const int ms) {
        return static_cast<std::size_t>(sampleRate) * ms / 1000;
    };
    const std::size_t warmupFrames = msToFrames(300);
    const std::size_t fadeFrames = msToFrames(20);
    const std::size_t silenceFrames = msToFrames(300);

    g_recordCapacityFrames = static_cast<std::size_t>(sampleRate) * recordSeconds;
    g_recordedSamples.assign(inChannels, std::vector<float>(g_recordCapacityFrames, 0.0f));
    g_recordedFrames.store(0);
    g_warmupFramesLeft.store(warmupFrames);

    // --- Phase 1 : enregistrement ---
    {
        mka::audio::JACK recorder;
        const mka::audio::EndpointConfig recordConfig {
            .id = in.id,
            .direction = mka::audio::Direction::Input,
            .inputChannels = inChannels,
            .outputChannels = 0,
            .sampleRate = sampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = in.bufferSize,
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
        mka::audio::JACK player;
        const mka::audio::EndpointConfig playbackConfig {
            .id = out.id,
            .direction = mka::audio::Direction::Output,
            .inputChannels = 0,
            .outputChannels = outChannels,
            .sampleRate = sampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = out.bufferSize,
        };

        ASSERT_TRUE(player.setProcessFunction(playbackFromRecordCallback));
        ASSERT_TRUE(player.open(playbackConfig));
        ASSERT_TRUE(player.start());

        // Stop uniquement une fois le silence final joué.
        ASSERT_TRUE(waitFor([] { return g_playbackPosition.load() >= g_playbackTotalFrames; },
                            static_cast<int>((g_playbackTotalFrames / sampleRate + 5) * 1000)));

        ASSERT_TRUE(player.stop());
        ASSERT_TRUE(player.close());
    }
}

//--- Test Duplex loopback (manuel, désactivé par défaut) --------------------

namespace {
    void loopbackCallback(void*, const mka::audio::AudioProcessContext &ctx) noexcept {
        if (ctx.input.count == 0) {
            silence(ctx);
            return;
        }
        for (std::uint32_t ch = 0; ch < ctx.output.count; ++ch) {
            const std::uint32_t src = std::min(ch, ctx.input.count - 1);
            for (std::uint32_t i = 0; i < ctx.frames; ++i) {
                ctx.output.channels[ch][i] = ctx.input.channels[src][i] * 0.5f;
            }
        }
    }
}

TEST(JackBackendTest, DISABLED_TestDuplexLoopback) {
    const auto both = discoverEndpoint(true, true);
    ASSERT_TRUE(both.available) << "aucun endpoint duplex JACK";
    std::println("duplex device: {}", both.id);

    mka::audio::JACK jack;
    const mka::audio::EndpointConfig config {
        .id = both.id,
        .direction = mka::audio::Direction::Duplex,
        .inputChannels = std::min<std::uint32_t>(2, both.inMax),
        .outputChannels = std::min<std::uint32_t>(2, both.outMax),
        .sampleRate = both.sampleRate,
        .format = mka::audio::Format::Float32,
        .bufferSize = both.bufferSize,
    };

    ASSERT_TRUE(jack.setProcessFunction(loopbackCallback));
    ASSERT_TRUE(jack.open(config));
    ASSERT_TRUE(jack.start());

    std::this_thread::sleep_for(std::chrono::seconds(5));

    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());
}