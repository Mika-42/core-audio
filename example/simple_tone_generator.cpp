#include <print>
#include <iostream>
#include <cmath>
#include <numbers>

import audio.alsa;

constexpr float FREQUENCY = 440.0f;
constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;
constexpr float ANGULAR = TWO_PI * FREQUENCY;

void audio_callback(const mka::audio::Block& block) {

	static float phase = 0.0f;

    const float phaseIncrement = ANGULAR / block.samplerate;

	for(uint32_t frame = 0; frame < block.frames; ++frame) {

		float sample = std::sin(phase);

		phase += phaseIncrement;
		
		if (phase >= TWO_PI) {
			 phase -= TWO_PI;
		}

		for(uint32_t channel = 0; channel < block.outChannels; ++channel) {
			block.out[channel][frame] = sample;
		}
	}
}

int main() 
{
	mka::audio::ALSA alsa;
	
	mka::audio::Config config {
		.samplerate = 44100,
		.bufferSize = 512,
		.outChannels = 1,       // stereo output
		.inChannels = 0,        // no input
        .audioFormat = mka::audio::Format::Float32,
		.name = "default"
	};

	mka::audio::Result ret = mka::audio::Ok;
   

	if(ret = alsa.open(config); !ret.ok()) {
        std::println("error::{}", ret.message);
        return -1;
    }

	alsa.setCallback(audio_callback);
	alsa.start();
	
	std::println("Press Enter to stop audio...\n");
	std::cin.get();
	
	alsa.stop();

	if(ret = alsa.close(); !ret.ok()) {
        std::println("error::{}", ret.message);
        return -1;
    }

	return 0;
}
