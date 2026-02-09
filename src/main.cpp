#include <print>
#include <iostream>
#include <cmath>

import audio.alsa;

void audio_callback(const mka::audio::Block& block) {
	static unsigned long long int i = 0;

	for(uint32_t f = 0; f < block.frames; ++f) {	
		for(uint32_t ch = 0; ch < block.channels; ++ch) {
			block.out[ch][f] = 0.5 * std::sin(2.0f * 3.1415926535f * 440.0f * (float)i / (float)block.samplerate); 
		}
		++i;
	}
}

int main() 
{

	mka::audio::ALSA alsa;
	
	mka::audio::Config config {
		.samplerate = 44100,
		.bufferSize = 512,
		.channels = 2,
		.name = "default"
	};

	auto ret = alsa.open(config);
	if(!ret.ok()) std::println("error::{}", ret.message);

	alsa.setCallback(audio_callback);
	alsa.start();
	
	std::println("Press Enter to stop audio...\n");
	std::cin.get();
	
	alsa.stop();

	ret = alsa.close();
	if(!ret.ok()) std::println("error::{}", ret.message);

	return 0;
}
