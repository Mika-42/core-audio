#include <print>
#include <iostream>
#include <cmath>

import audio.jack;

void audio_callback(const mka::audio::Block& block) {
	static float phase = {};

	for(uint32_t i = 0; i < block.frames; ++i) {
		float sample = sinf(phase);

		phase += 2.0f * M_PI * 440.00f / block.sampleRate;

		for(uint32_t ch = 0; ch < block.outChannels; ++ch) {
			block.out[ch][i] = sample;
		}
	}
}

int main() 
{

	mka::audio::JACK engine;
	
	auto l = engine.getChannels();
    for (auto& ch : l) {
		
		std::println("{{");
		std::println("\tdevice name:\t{},", ch.deviceName);
		std::println("\tname:\t\t{},", ch.name);
		std::println("\tport:\t\t{},", ch.port);
		std::println("\tsamplerate:\t{},", *ch.sampleRate);
		std::println("\tbuffer size:\t{},", *ch.bufferSize);
		std::println("\tis input:\t{}", ch.input);
		std::println("}}\n");
    }
	return 0;
	
/*	auto ret = engine.open(config);
	if(!ret.ok()) {
		std::println("error::{}", ret.message);
		return 1;
	}
	engine.setCallback(audio_callback);
	engine.start();
	
	std::println("Press Enter to stop audio...\n");
	std::cin.get();
	
	engine.stop();

	ret = engine.close();
	if(!ret.ok()) std::println("error::{}", ret.message);

	return 0;
	*/
}

