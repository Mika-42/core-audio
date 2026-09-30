//
// Tests "contrat temps réel" JACK : B7 (sortie à zéro) et absence d'allocation.
//
// Tous les tests utilisent id = "" : les ports sont enregistrés mais jamais
// connectés au matériel, donc rien n'est envoyé aux enceintes.
//
#include <gtest/gtest.h>
#include <jack/jack.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <memory>

#include "utils/rt_test_utils.hpp"

import mka.audio.backend.jack;
import mka.audio.process;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;

namespace {
    void outputCallback(void* user, const mka::audio::AudioProcessContext& ctx) noexcept {
        rt_test::dirtyOutput(*static_cast<rt_test::ContractState*>(user), ctx);
    }

    struct ServerParams {
        bool ok = false;
        mka::audio::SampleRate sampleRate = 0;
        mka::audio::BufferSize bufferSize = 0;
    };

    // Rate et buffer sont imposés par le serveur : on les lit pour construire une config valide.
    ServerParams queryServer() {
        jack_status_t status{};
        jack_client_t* c = jack_client_open("mka-rt-params", JackNoStartServer, &status);
        if (!c) return {};

        ServerParams p{ .ok = true, .sampleRate = jack_get_sample_rate(c), .bufferSize = jack_get_buffer_size(c) };
        jack_client_close(c);

        const auto has = [](const auto& arr, const auto v) { return std::ranges::find(arr, v) != arr.end(); };
        p.ok = has(mka::audio::supportedSampleRates, p.sampleRate)
            && has(mka::audio::supportedBufferSizes, p.bufferSize);
        return p;
    }

    mka::audio::EndpointConfig makeConfig(const ServerParams& p) {
        return mka::audio::EndpointConfig{
            .id = "",
            .direction = mka::audio::Direction::Output,
            .inputChannels = 0,
            .outputChannels = 2,
            .sampleRate = p.sampleRate,
            .format = mka::audio::Format::Float32,
            .bufferSize = p.bufferSize,
        };
    }

    // --- Client sonde : mesure le pic du signal reçu de notre backend ----------
    struct Probe {
        jack_port_t* port = nullptr;
        std::atomic<int> cycles{0};
        std::atomic<float> peak{0.0f};
    };

    int probeProcess(const jack_nframes_t nframes, void* arg) {
        auto* p = static_cast<Probe*>(arg);
        const auto* buf = static_cast<const float*>(jack_port_get_buffer(p->port, nframes));
        float peak = p->peak.load(std::memory_order_relaxed);
        for (jack_nframes_t i = 0; i < nframes; ++i) peak = std::max(peak, std::fabs(buf[i]));
        p->peak.store(peak, std::memory_order_relaxed);
        p->cycles.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    using ClientPtr = std::unique_ptr<jack_client_t, decltype(&jack_client_close)>;
}

class JackRtContractTest : public ::testing::Test {
    protected:
        static ServerParams server;
        static void SetUpTestSuite() { server = queryServer(); }

        void SetUp() override {
            if (!server.ok) {
                GTEST_SKIP() << "serveur JACK absent, ou rate/buffer hors valeurs supportées";
            }
        }
};
ServerParams JackRtContractTest::server;

// B7 : à l'entrée du callback, la sortie vaut toujours 0.
TEST_F(JackRtContractTest, OutputIsZeroedBeforeCallback) {
    rt_test::ContractState state;
    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(outputCallback, &state));
    ASSERT_TRUE(jack.open(makeConfig(server)));
    ASSERT_TRUE(jack.start());

    const bool enough = rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });

    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());

    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();
    EXPECT_TRUE(state.sawOutput.load());
    EXPECT_FALSE(state.outputDirtyOnEntry.load())
        << "la sortie n'était pas à zéro à l'entrée du callback";
}

// Aucune allocation C++ hors du thread de test entre start() et stop().
TEST_F(JackRtContractTest, NoHeapAllocationOnAudioThread) {
    alloc_probe::ignoreCurrentThread();

    rt_test::ContractState state;
    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(outputCallback, &state));
    ASSERT_TRUE(jack.open(makeConfig(server)));

    alloc_probe::arm();
    const auto started = jack.start();
    const bool enough = started
        && rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });
    const auto stopped = started ? jack.stop() : mka::audio::Result{};
    const std::size_t allocations = alloc_probe::disarm();

    ASSERT_TRUE(started);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(jack.close());
    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();

    EXPECT_EQ(allocations, 0u) << "allocation(s) C++ détectée(s) sur un thread audio";
}

// B7 sans callback : le signal réellement émis est du silence, même si le
// cycle d'avant (phase 1, callback qui salit les buffers) avait écrit autre chose.
TEST_F(JackRtContractTest, NoCallbackProducesSilence) {
    // Phase 1 : salit les buffers de sortie.
    rt_test::ContractState dirty;
    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(outputCallback, &dirty));
    ASSERT_TRUE(jack.open(makeConfig(server)));
    ASSERT_TRUE(jack.start());
    ASSERT_TRUE(rt_test::waitFor([&] { return dirty.calls.load() >= 20; }));
    ASSERT_TRUE(jack.stop());

    // Phase 2 : plus de callback.
    ASSERT_TRUE(jack.setProcessFunction(nullptr));
    ASSERT_TRUE(jack.start());

    // Client sonde branché sur nos ports de sortie.
    Probe probe;
    jack_status_t status{};
    ClientPtr client{ jack_client_open("mka-rt-probe", JackNoStartServer, &status), &jack_client_close };
    ASSERT_TRUE(client) << "impossible d'ouvrir le client sonde";

    probe.port = jack_port_register(client.get(), "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    ASSERT_NE(probe.port, nullptr);
    ASSERT_EQ(jack_set_process_callback(client.get(), &probeProcess, &probe), 0);
    ASSERT_EQ(jack_activate(client.get()), 0);

    const char** ports = jack_get_ports(client.get(), "mka-audio:", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput);
    ASSERT_NE(ports, nullptr) << "ports de sortie du backend introuvables";
    ASSERT_NE(ports[0], nullptr);
    const int connected = jack_connect(client.get(), ports[0], jack_port_name(probe.port));
    jack_free(ports);
    ASSERT_TRUE(connected == 0 || connected == EEXIST);

    probe.cycles.store(0);
    probe.peak.store(0.0f);
    ASSERT_TRUE(rt_test::waitFor([&] { return probe.cycles.load() >= rt_test::kMinCycles; }));

    jack_deactivate(client.get());
    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());

    EXPECT_EQ(probe.peak.load(), 0.0f) << "signal non nul émis sans callback";
}

namespace {
    void countCallback(void* user, const mka::audio::AudioProcessContext& ctx) noexcept {
        rt_test::countAudioCall(*static_cast<rt_test::StopContractState*>(user), ctx);
    }
}

// Aucun callback après le retour de stop(), sur des start/stop répétés avec
// des lectures concurrentes de status().
TEST_F(JackRtContractTest, NoCallbackAfterStopUnderStress) {
    rt_test::StopContractState state;
    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(countCallback, &state));
    ASSERT_TRUE(jack.open(makeConfig(server)));

    const char* failure = rt_test::stopStress(jack, state);
    ASSERT_TRUE(jack.close());
    EXPECT_STREQ(failure, "");
}

// Aucun malloc/free (y compris dans libjack) sur le thread audio en régime établi.
TEST_F(JackRtContractTest, NoLibcAllocationOnAudioThread) {
    rt_test::StopContractState state;
    mka::audio::JACK jack;
    ASSERT_TRUE(jack.setProcessFunction(countCallback, &state));
    ASSERT_TRUE(jack.open(makeConfig(server)));
    ASSERT_TRUE(jack.start());

    // Chauffe : les premiers cycles ne sont pas mesurés.
    const bool warm = rt_test::waitFor([&] { return state.calls.load() >= 8; });
    alloc_probe::armAudio();
    const int from = state.calls.load();
    const bool enough = warm
        && rt_test::waitFor([&] { return state.calls.load() >= from + rt_test::kMinCycles; });
    const std::size_t allocations = alloc_probe::disarmAudio();

    ASSERT_TRUE(jack.stop());
    ASSERT_TRUE(jack.close());
    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();
    EXPECT_EQ(allocations, 0u) << "malloc/free détecté(s) sur le thread audio";
}
