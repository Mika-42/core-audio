#include <print>
#include <iostream>
#include <limits>
#include <cmath>

//import audio.abstract_core;
//import audio.pipewire;
import audio.engine;
import audio.block;
import audio.routing_table;
//using namespace mka::audio; 
/*
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
*/

namespace mka::audio {
	void processNextBlock(const BlockView& input, BlockView& output) {
		for(size_t i=0; i<output.frames(); ++i) {
			output.channel(0)[i] = std::cos(i);
		}
	}
}

int main() {
/*	mka::audio::BackendBuffer input_buffer {
		.data = nullptr,
		.channels = 0,
		.frames = 512
	};

float output_buffer_data[64][4096] = {};
float* output_channels[64];

for (size_t ch = 0; ch < 64; ++ch) {
    output_channels[ch] = output_buffer_data[ch];
}
*/
	//mka::audio::BackendBuffer output_buffer {
	//	.data = output_channels,
	//	.channels = 64,
	//	.frames = 4096
	//};

	//mka::audio::RoutingTable routing(64);

//	routing.output(0).link(0);

	//mka::audio::Block workIn_ {};
	//mka::audio::Block workOut_ {};
	//workIn_.configure(64, 512);
	//workOut_.configure(64, 512);
	//mka::audio::audio_process(input_buffer, output_buffer, routing,workIn_, workOut_, mka::audio::processNextBlock);
	/*	
	PipeWire engine;

	const auto devices = engine.enumerateDevices();
	print_devices(devices);

	int choice = -1;
	std::print(">> choisis le device cible: ");
	std::cin >> choice;

	mka::audio::DeviceConfig cfg {
		.sampleRate = 44100,
		.inputChannels = 16,
		.outputChannels = 16,
	};

	CHECK(engine.open(cfg));
	
	auto outPorts = engine.enumeratePorts(devices[choice].nodeName);
	std::println("Ports du device de sortie '{}':", devices[choice].description);
	
	for (auto& p : outPorts) {
		if (p.isInput) std::println("  {} ({})", p.name, p.isInput ? "out" : "in");
	}

	int choice2 = -1;
	if(outPorts.size() != 0) {
		std::print(">> choisis le port cible: ");
		std::cin >> choice2;
	}

	if (choice2 >= 0 && choice2 < static_cast<int>(outPorts.size())) {
		CHECK(engine.routePort(
			"mka_audio_out",
			PipeWire::auxPortName(PipeWire::Direction::Output, 4),
			devices[choice].nodeName,
			outPorts[choice2].name
		));
	}


	// [2] setup the engine
	engine.setCallback(audio_callback);
	
	// [4] start the engine
	CHECK(engine.start());

	std::println("press any key to stop audio...");
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	std::cin.get();

	// [5] stop the engine
	CHECK(engine.stop());
	
	// [6] close the engine
	CHECK(engine.close());
*/
	return 0;
}

