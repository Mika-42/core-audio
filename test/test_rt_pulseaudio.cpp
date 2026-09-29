//
// Tests "contrat temps réel" PulseAudio : B7 (sortie à zéro) et absence d'allocation.
//
// Attention : le flux est envoyé au premier sink PulseAudio trouvé. Le signal
// "sale" est un continu à -60 dBFS (rt_test::kDirtyValue), donc inoffensif.
//
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <string>

#include "utils/rt_test_utils.hpp"

import mka.audio.backend.pulseaudio;
import mka.audio.process;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;

namespace {
    constexpr mka::audio::SampleRate kRate = 48'000;
    constexpr mka::audio::BufferSize kBuffer = 512;
    constexpr std::uint32_t kChannels = 2;

    void outputCallback(void* user, const mka::audio::AudioProcessContext& ctx) noexcept {
        rt_test::dirtyOutput(*static_cast<rt_test::ContractState*>(user), ctx);
    }

    std::string discoverOutputId() {
        const mka::audio::PulseAudio pa;
        for (const auto& e : pa.getEndPoints()) {
            if (e.output.has_value()) return e.id;
        }
        return {};
    }
}

class PulseAudioRtContractTest : public ::testing::Test {
    protected:
        static std::string outputId;
        static void SetUpTestSuite() { outputId = discoverOutputId(); }

        static mka::audio::EndpointConfig makeConfig() {
            return mka::audio::EndpointConfig{
                .id = outputId,
                .direction = mka::audio::Direction::Output,
                .inputChannels = 0,
                .outputChannels = kChannels,
                .sampleRate = kRate,
                .format = mka::audio::Format::Float32,
                .bufferSize = kBuffer,
            };
        }

        void SetUp() override {
            if (outputId.empty()) GTEST_SKIP() << "aucun sink PulseAudio disponible";
        }
};
std::string PulseAudioRtContractTest::outputId;

// B7 : la sortie est à zéro à l'entrée du callback. Le scratch_ du backend
// persiste d'un appel à l'autre : sans remise à zéro, le 2e appel voit du sale.
TEST_F(PulseAudioRtContractTest, OutputIsZeroedBeforeCallback) {
    rt_test::ContractState state;
    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.setProcessFunction(outputCallback, &state));
    ASSERT_TRUE(pa.open(makeConfig()));
    ASSERT_TRUE(pa.start());

    const bool enough = rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });

    ASSERT_TRUE(pa.stop());
    ASSERT_TRUE(pa.close());

    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();
    EXPECT_TRUE(state.sawOutput.load());
    EXPECT_FALSE(state.outputDirtyOnEntry.load())
        << "la sortie n'était pas à zéro à l'entrée du callback";
}

// Aucune allocation C++ hors du thread de test entre start() et stop().
TEST_F(PulseAudioRtContractTest, NoHeapAllocationOnAudioThread) {
    alloc_probe::ignoreCurrentThread();

    rt_test::ContractState state;
    mka::audio::PulseAudio pa;
    ASSERT_TRUE(pa.setProcessFunction(outputCallback, &state));
    ASSERT_TRUE(pa.open(makeConfig()));

    alloc_probe::arm();
    const auto started = pa.start();
    const bool enough = started
        && rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });
    const auto stopped = started ? pa.stop() : mka::audio::Result{};
    const std::size_t allocations = alloc_probe::disarm();

    ASSERT_TRUE(started);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(pa.close());
    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();

    EXPECT_EQ(allocations, 0u) << "allocation(s) C++ détectée(s) sur un thread audio";
}
