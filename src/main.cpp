#include <print>
#include <iostream>
#include <cmath>

import audio.jack;

void audio_callback(mka::audio::Block& block) {
	static float phase = {};
	constexpr float twoPi = 2.0f * static_cast<float>(M_PI);
	const float phaseIncrement = twoPi * 440.0f / static_cast<float>(block.sampleRate);

	for(uint32_t i = 0; i < block.blockSize; ++i) {
		float sample = sinf(phase);

		phase += phaseIncrement;
		if (phase >= twoPi) {
			// Keep the phase bounded to avoid long-running precision loss (pitch drift/jitter).
			phase -= twoPi;
		}

		for(uint32_t ch = 0; ch < block.outputCount; ++ch) {
			block.outputs[ch][i] = sample;
		}
	}
}

void printChannels(const std::vector<mka::audio::ChannelInfo>& channels) {
    for (auto& ch : channels) {
		
		std::println("{{");
		std::println("\tchannel name:\t\t{},", ch.name);
		std::println("\tdirection:\t{}", ch.direction == mka::audio::Direction::In ? "Input" : "Output");
		std::println("}}\n");
    }
}

int main() {
	mka::audio::JACK engine;
	
	auto l = engine.getChannels();
	printChannels(l);
	int choice = 0;
	std::cout << ">> enter number: ";
	std::cin >> choice;

	engine.setCallback(audio_callback);
	//engine.setSampleRate(48'000);

	engine.open(l[choice]);
	engine.start();
	
	std::cout << "press any key to exit.";
	std::cin >> choice;
	engine.stop();
	engine.close();
	return 0;
}

