#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.jack;

void audio_callback(mka::audio::Block& audioBlock, const mka::midi::Block& midiBlock) {
	(void)midiBlock;
	static float phase = {};
	constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;
	const float phaseIncrement = twoPi * 440.0f / static_cast<float>(audioBlock.sampleRate);

	for(uint32_t i = 0; i < audioBlock.blockSize; ++i) {
		float sample = sinf(phase);

		phase += phaseIncrement;
		if (phase >= twoPi) {
			phase -= twoPi;
		}

		for(uint32_t ch = 0; ch < audioBlock.outputCount; ++ch) {
			audioBlock.outputs[ch][i] = sample;
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
	engine.setSampleRate(48'000);
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

