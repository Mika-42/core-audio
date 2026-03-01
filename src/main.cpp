#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.jack;

void audio_callback(mka::audio::Block& block, const mka::audio::MidiEventBlock& midiEvents) {
	static float phase = {};
	constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;
	const float phaseIncrement = twoPi * 440.0f / static_cast<float>(block.sampleRate);

	// MIDI events are already aligned to the current audio block.
	for (uint32_t eventIndex = 0; eventIndex < midiEvents.eventCount; ++eventIndex) {
		const auto& event = midiEvents.events[eventIndex];
		// Example: simple NOTE ON diagnostic (status 0x90, velocity > 0).
		if (event.size >= 3 && (event.data[0] & 0xF0) == 0x90 && event.data[2] > 0) {
			std::println("MIDI note on ch={} note={} vel={} frame={}",
				(event.data[0] & 0x0F) + 1,
				event.data[1],
				event.data[2],
				event.frameOffset);
		}
	}

	for(uint32_t i = 0; i < block.blockSize; ++i) {
		float sample = sinf(phase);

		phase += phaseIncrement;
		if (phase >= twoPi) {
			phase -= twoPi;
		}

		for(uint32_t ch = 0; ch < block.outputCount; ++ch) {
			block.outputs[ch][i] = sample;
		}
	}
}

int main() {
	mka::audio::JACK engine;
	
	// [0] list all availables channels
	auto channels = engine.getChannels();
    for (auto& ch : channels) {	
		std::println("channel name:\t{}", ch.name);
		std::println("direction:\t{}\n", ch.direction == mka::audio::Direction::In ? "Input" : "Output");
    }
	
	// [1] select one of them
	int choice = 0;
	std::print(">> enter number: ");
	std::cin >> choice;
	
	// [2] setup the engine
	engine.setCallback(audio_callback);
	engine.setSampleRate(22'050);
	engine.setBlockSize(1024);

	// [3] open the desire channel
	auto ret = engine.open(channels[choice]);
	if(!ret.ok()) {
        std::println("error::{}", ret.message);
        return -1;
    }

	// [4] start the engine
	engine.start();
	
	std::println("press any key to stop audio...");
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	std::cin.get();

	// [5] stop the engine
	engine.stop();
	
	// [] close the engine
	ret = engine.close();
	if(!ret.ok()) {
        std::println("error::{}", ret.message);
        return -1;
    }

	return 0;
}

