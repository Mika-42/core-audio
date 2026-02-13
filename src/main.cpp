#include <print>
#include <iostream>
#include <cmath>

import audio.alsa;

void audio_callback(const mka::audio::Block& block) {
	static float phase = {};

	for(uint32_t i = 0; i < block.frames; ++i) {
		float sample = sinf(phase);

		phase += 2.0f * M_PI * 440.00f / block.samplerate;

		for(uint32_t ch = 0; ch < block.outChannels; ++ch) {
			block.out[ch][i] = sample;
		}
	}
}

int main() 
{

	mka::audio::ALSA engine;
	
	mka::audio::Config config {
		.samplerate = 44100,
		.bufferSize = 512,
		.outChannels = 1,
		.inChannels = 0,
		.audioFormat = mka::audio::Format::Float32,
		.name = "default"
	};

	auto ret = engine.open(config);
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
}
