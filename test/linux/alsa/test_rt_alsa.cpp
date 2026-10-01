//
// Tests "contrat temps réel" ALSA : B7 (sortie à zéro) et absence d'allocation.
//
// Même configuration que test_alsa.cpp (duplex, Int32, 44100, 512). Le device
// se change avec la variable d'environnement MKA_TEST_ALSA_DEVICE (défaut hw:1,0).
// Les tests sont ignorés si le device ne peut pas être ouvert.
//
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

#include "../../utils/rt_test_utils.hpp"

import mka.audio.backend.alsa;
import mka.audio.process;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;

namespace {
    void outputCallback(void* user, const mka::audio::AudioProcessContext& ctx) noexcept {
        rt_test::dirtyOutput(*static_cast<rt_test::ContractState*>(user), ctx);
    }

    mka::audio::EndpointConfig makeConfig() {
        const char* env = std::getenv("MKA_TEST_ALSA_DEVICE");
        return mka::audio::EndpointConfig{
            .id = env ? env : "hw:1,0",
            .direction = mka::audio::Direction::Duplex,
            .inputChannels = 2,
            .outputChannels = 2,
            .sampleRate = 44100,
            .format = mka::audio::Format::Int32,
            .bufferSize = 512,
        };
    }
}

// B7 : la sortie est à zéro à l'entrée du callback. outputScratch_ persiste
// d'un cycle à l'autre : sans remise à zéro, le 2e cycle voit du sale.
TEST(ALSARtContractTest, OutputIsZeroedBeforeCallback) {
    rt_test::ContractState state;
    mka::audio::ALSA alsa;
    ASSERT_TRUE(alsa.setProcessFunction(outputCallback, &state));

    if (!alsa.open(makeConfig())) GTEST_SKIP() << "device ALSA indisponible";
    ASSERT_TRUE(alsa.start());

    const bool enough = rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });

    ASSERT_TRUE(alsa.stop());
    ASSERT_TRUE(alsa.close());

    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();
    EXPECT_TRUE(state.sawOutput.load());
    EXPECT_FALSE(state.outputDirtyOnEntry.load())
        << "la sortie n'était pas à zéro à l'entrée du callback";
}

// Aucune allocation C++ hors du thread de test entre start() et stop().
TEST(ALSARtContractTest, NoHeapAllocationOnAudioThread) {
    alloc_probe::ignoreCurrentThread();

    rt_test::ContractState state;
    mka::audio::ALSA alsa;
    ASSERT_TRUE(alsa.setProcessFunction(outputCallback, &state));

    if (!alsa.open(makeConfig())) GTEST_SKIP() << "device ALSA indisponible";

    alloc_probe::arm();
    const auto started = alsa.start();
    const bool enough = started
        && rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });
    const auto stopped = started ? alsa.stop() : mka::audio::Result{};
    const std::size_t allocations = alloc_probe::disarm();

    ASSERT_TRUE(started);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(alsa.close());
    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();

    EXPECT_EQ(allocations, 0u) << "allocation(s) C++ détectée(s) sur un thread audio";
}
