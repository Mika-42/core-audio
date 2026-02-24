#include <print>
#include <iostream>
#include <cmath>

import audio.jack;

void audio_callback(mka::audio::Block& block) {
	static float phase = {};

	for(uint32_t i = 0; i < block.blockSize; ++i) {
		float sample = sinf(phase);

		phase += 2.0f * M_PI * 440.00f / block.sampleRate;

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

int main() 
{

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
	while(true) {}
	engine.stop();
	engine.close();
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

