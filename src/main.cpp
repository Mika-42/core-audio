#include <print>
#include <iostream>
#include <cmath>

import audio.alsa;

void audio_callback(const mka::audio::Block& block) {
	static float phase = 0.0f;

	for(uint32_t i = 0; i < block.frames; ++i) {
		float sample = sinf(phase);
		phase += 2.0f * M_PI * 440.0f / block.samplerate;
		for(uint32_t ch = 0; ch < block.outChannels; ++ch) {
			block.out[ch][i] = sample;
		}
	}
}

int main() 
{

	mka::audio::ALSA alsa;
	
	mka::audio::Config config {
		.samplerate = 44100,
		.bufferSize = 512,
		.outChannels = 2,
		.inChannels = 0,
		.name = "default"
	};

	auto ret = alsa.open(config);
	if(!ret.ok()) {
		std::println("error::{}", ret.message);
		return 1;
	}
	alsa.setCallback(audio_callback);
	alsa.start();
	
	std::println("Press Enter to stop audio...\n");
	std::cin.get();
	
	alsa.stop();

	ret = alsa.close();
	if(!ret.ok()) std::println("error::{}", ret.message);

	return 0;
}
