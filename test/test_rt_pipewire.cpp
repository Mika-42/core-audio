//
// Tests "contrat temps réel" PipeWire : correctifs B6 et B7.
//
//  - OutputIsZeroedBeforeCallback   : B7 (sortie à zéro avant le callback)
//  - NoHeapAllocationOnAudioThread* : B6 (aucune allocation dans onProcess)
//
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <string>

#include "utils/rt_test_utils.hpp"

import mka.audio.backend.pipewire;
import mka.audio.process;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;

namespace {
    void outputCallback(void* user, const mka::audio::AudioProcessContext& ctx) noexcept {
        rt_test::dirtyOutput(*static_cast<rt_test::ContractState*>(user), ctx);
    }

    void inputCallback(void* user, const mka::audio::AudioProcessContext& ctx) noexcept {
        rt_test::touchInput(*static_cast<rt_test::ContractState*>(user), ctx);
    }

    struct Found {
        bool available = false;
        std::string id;
        mka::audio::SampleRate sampleRate = 0;
        mka::audio::BufferSize bufferSize = 0;
        std::uint32_t channels = 0;
    };

    Found discover(const bool wantInput) {
        const mka::audio::PipeWire pw;
        for (const auto& e : pw.getEndPoints()) {
            const auto& caps = wantInput ? e.input : e.output;
            if (!caps || caps->sampleRates.empty() || caps->bufferSizes.empty()
                || caps->maxChannels == 0) {
                continue;
            }
            return Found{
                .available = true,
                .id = e.id,
                .sampleRate = caps->sampleRates.front(),
                .bufferSize = caps->bufferSizes.front(),
                .channels = std::min<std::uint32_t>(2, caps->maxChannels),
            };
        }
        return {};
    }
}

class PipeWireRtContractTest : public ::testing::Test {
    protected:
        static Found output;
        static Found input;

        static void SetUpTestSuite() {
            output = discover(false);
            input = discover(true);
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

        static mka::audio::EndpointConfig makeInputConfig() {
            return mka::audio::EndpointConfig{
                .id = input.id,
                .direction = mka::audio::Direction::Input,
                .inputChannels = input.channels,
                .outputChannels = 0,
                .sampleRate = input.sampleRate,
                .format = mka::audio::Format::Float32,
                .bufferSize = input.bufferSize,
            };
        }
};
Found PipeWireRtContractTest::output;
Found PipeWireRtContractTest::input;

// B7 : à l'entrée du callback, la sortie vaut toujours 0, même si le cycle
// précédent (ou le buffer recyclé du pool PipeWire) contenait autre chose.
TEST_F(PipeWireRtContractTest, OutputIsZeroedBeforeCallback) {
    if (!output.available) GTEST_SKIP() << "aucun endpoint de sortie PipeWire";

    rt_test::ContractState state;
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.setProcessFunction(outputCallback, &state));
    ASSERT_TRUE(pw.open(makeOutputConfig()));
    ASSERT_TRUE(pw.start());

    const bool enough = rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });

    ASSERT_TRUE(pw.stop());
    ASSERT_TRUE(pw.close());

    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();
    EXPECT_TRUE(state.sawOutput.load());
    EXPECT_FALSE(state.outputDirtyOnEntry.load())
        << "la sortie n'était pas à zéro à l'entrée du callback";
}

// B6 (sortie) : aucune allocation C++ hors du thread de test entre start() et stop().
// Couvre aussi la première itération de onProcess (ancien static thread_local + resize).
TEST_F(PipeWireRtContractTest, NoHeapAllocationOnAudioThreadOutput) {
    if (!output.available) GTEST_SKIP() << "aucun endpoint de sortie PipeWire";

    alloc_probe::ignoreCurrentThread();

    rt_test::ContractState state;
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.setProcessFunction(outputCallback, &state));
    ASSERT_TRUE(pw.open(makeOutputConfig()));

    alloc_probe::arm();
    const auto started = pw.start();
    const bool enough = started
        && rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });
    const auto stopped = started ? pw.stop() : mka::audio::Result{};
    const std::size_t allocations = alloc_probe::disarm();

    ASSERT_TRUE(started);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(pw.close());
    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();

    EXPECT_EQ(allocations, 0u) << "allocation(s) C++ détectée(s) sur un thread audio";
}

// B6 (entrée) : même contrôle sur le chemin capture.
TEST_F(PipeWireRtContractTest, NoHeapAllocationOnAudioThreadInput) {
    if (!input.available) GTEST_SKIP() << "aucun endpoint d'entrée PipeWire";

    alloc_probe::ignoreCurrentThread();

    rt_test::ContractState state;
    mka::audio::PipeWire pw;
    ASSERT_TRUE(pw.setProcessFunction(inputCallback, &state));
    ASSERT_TRUE(pw.open(makeInputConfig()));

    alloc_probe::arm();
    const auto started = pw.start();
    const bool enough = started
        && rt_test::waitFor([&] { return state.calls.load() >= rt_test::kMinCycles; });
    const auto stopped = started ? pw.stop() : mka::audio::Result{};
    const std::size_t allocations = alloc_probe::disarm();

    ASSERT_TRUE(started);
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(pw.close());
    ASSERT_TRUE(enough) << "pas assez de cycles observés : " << state.calls.load();

    EXPECT_EQ(allocations, 0u) << "allocation(s) C++ détectée(s) sur un thread audio";
}
