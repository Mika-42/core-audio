#include <print>
#include <iostream>
#include <cmath>

import audio.jack;

void audio_callback(const mka::audio::ChannelInfo& info) {
	static float phase = {};

	for(uint32_t i = 0; i < info.frameCount; ++i) {
		float sample = sinf(phase);

		phase += 2.0f * M_PI * 440.00f / info.sampleRate;

		for(uint32_t ch = 0; ch < info.output.channelCount; ++ch) {
			info.output.data[ch][i] = sample;
		}
	}
}

void printChannels(const std::vector<mka::audio::Channel>& channels) {
    for (auto& ch : channels) {
		
		std::println("{{");
		std::println("\tdevice name:\t{},", ch.deviceName);
		std::println("\tname:\t\t{},", ch.name);
		std::println("\tport:\t\t{},", ch.port);
		std::println("\tsamplerate:\t{},", *ch.sampleRate);
		std::println("\tbuffer size:\t{},", *ch.bufferSize);
		std::println("\tis input:\t{}", ch.input);
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

