#include <print>
#include <iostream>
#include <limits>
#include <cmath>

import audio.abstract_core;
import audio.pipewire;

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
		std::println("nodeName : {}, description : {}, sink : {}, source : {}", d.nodeName, d.description, d.isSink, d.isSource);
	}
}

int main() {
	mka::audio::PipeWire engine;

	const auto devices = engine.enumerateDevices();
	print_devices(devices);

	// [2] setup the engine
	engine.setCallback(audio_callback);

	// [3] open the desire channel
	// output 0 : master L
	// output 1 : master R
	// output 2 : headphone L
	// output 3 : headphone R
	// output 4
	// output 5
	// output 6
	// output 7
	// output 8
	// output 9
	// output 10
	// output 11
	// output 12
	// output 13
	// output 14
	// output 15
	mka::audio::DeviceConfig cfg {
		.sampleRate = 44100,
		.inputChannels = 16,
		.outputChannels = 16,
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

