#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.abstract_core;
import audio.alsa;

#define CHECK(x)								\
do {											\
	auto ret = x;								\
	if(!ret.ok()) {								\
        std::println("error::{}", ret.message); \
        return -1;								\
    }											\
} while(0)

void audio_callback(mka::audio::Buffer& buffer, void*) {
	static uint64_t idx = 0;
	constexpr float twoPi = 2.0f * std::numbers::pi_v<float>;

	for(uint32_t i = 0; i < buffer.frames; ++i, ++idx) {

		float sample = std::cos(twoPi * 440.0f * (float)idx / 44100.0f);
		for(uint32_t ch = 0; ch < buffer.outputCount; ++ch) {
			buffer.outputs[ch][i] = sample;
		}
	}
}

void print_info(const mka::audio::DeviceInfo& info) {

	std::println(
			"[\n\tid: {}\n\tname: {}\n\tsample rate: {}\n\tbuffer size: {}\n\tinput channels: {}\n\toutput channels: {}\n\tinput latency (ms): {}\n\toutput latency (ms): {}\n\tsample format : {}\n]", 
			info.id, 
			info.name, 
			info.sampleRate, 
			info.bufferSize, 
			info.inputChannels, 
			info.outputChannels, 
			info.inputLatencyMs,
			info.outputLatencyMs, 
			(int)info.sampleFormat
	);
}

void print_devices(const auto &devices) {
	for(const auto& d : devices) {
		std::println("hardwareID : {}, name : {}", d.hardwareID, d.name);
	}
}

int main() {
	mka::audio::ALSA engine;

	const auto devices = engine.enumerateDevices();
	print_devices(devices);

	// [1] select one of them
	int choice = 0;
	std::print(">> enter number: ");
	std::cin >> choice;

	// [2] setup the engine
	engine.setCallback(audio_callback);

	// [3] open the desire channel
	mka::audio::DeviceConfig cfg {
		.deviceID = devices[choice].hardwareID,
		.sampleRate = 44100,
		.inputChannels = 0,
		.outputChannels = 2,

	};

	CHECK(engine.open(cfg));

	print_info(engine.info());

	// [4] start the engine
	CHECK(engine.start());

	std::println("press any key to stop audio...");
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	std::cin.get();

	// [5] stop the engine
	CHECK(engine.stop());
	
	// [6] close the engine
	CHECK(engine.close());

	return 0;
}


