module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <jack/jack.h>

export module audio.jack;
export import audio.block;
export import audio.config;

import audio.error;
import audio.abstract_core;

export namespace mka::audio {

	class JACK final: public AbstractCoreAudio {
	public:
		JACK() = default;

		Result open(const Config& cfg) override {
			config = cfg;

			jack_status_t status = JackServerFailed;
			const char* clientName = config.name.empty() ? "mka-core-audio" : config.name.c_str();
			client = jack_client_open(clientName, JackNullOption, &status);
			if(client == nullptr) {
				return Result { Error::DeviceOpenFailed, "jack_client_open failed" };
			}

			jack_on_shutdown(client, &JACK::onShutdown, this);
			if(jack_set_process_callback(client, &JACK::processTrampoline, this) != 0) {
				jack_client_close(client);
				client = nullptr;
				return Result { Error::SetupHardwareParameterFailed, "jack_set_process_callback failed" };
			}

			config.samplerate = static_cast<uint32_t>(jack_get_sample_rate(client));
			config.bufferSize = static_cast<uint32_t>(jack_get_buffer_size(client));
			config.audioFormat = Format::Float32;

			outPorts.clear();
			inPorts.clear();
			outPorts.reserve(config.outChannels);
			inPorts.reserve(config.inChannels);
			outPtrs.resize(config.outChannels, nullptr);
			inPtrs.resize(config.inChannels, nullptr);

			for(uint32_t ch = 0; ch < config.outChannels; ++ch) {
				char portName[32] = {};
				std::snprintf(portName, sizeof(portName), "out_%u", ch + 1);
				jack_port_t* port = jack_port_register(
					client,
					portName,
					JACK_DEFAULT_AUDIO_TYPE,
					JackPortIsOutput,
					0
				);
				if(port == nullptr) {
					close();
					return Result { Error::SetupHardwareParameterFailed, "jack_port_register output failed" };
				}
				outPorts.push_back(port);
			}

			for(uint32_t ch = 0; ch < config.inChannels; ++ch) {
				char portName[32] = {};
				std::snprintf(portName, sizeof(portName), "in_%u", ch + 1);
				jack_port_t* port = jack_port_register(
					client,
					portName,
					JACK_DEFAULT_AUDIO_TYPE,
					JackPortIsInput,
					0
				);
				if(port == nullptr) {
					close();
					return Result { Error::SetupHardwareParameterFailed, "jack_port_register input failed" };
				}
				inPorts.push_back(port);
			}

			return Ok;
		}

		Result close() override {
			if(client != nullptr) {
				jack_deactivate(client);
				jack_client_close(client);
				client = nullptr;
			}

			outPorts.clear();
			inPorts.clear();
			outPtrs.clear();
			inPtrs.clear();
			return Ok;
		}

	protected:
		void run() override {
			if(client == nullptr) {
				running.store(false, std::memory_order_release);
				return;
			}

			if(jack_activate(client) != 0) {
				running.store(false, std::memory_order_release);
				return;
			}

			while(running.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}

			jack_deactivate(client);
		}

	private:
		static int processTrampoline(jack_nframes_t nframes, void* arg) {
			return static_cast<JACK*>(arg)->process(nframes);
		}

		int process(jack_nframes_t nframes) {
			for(uint32_t ch = 0; ch < config.outChannels; ++ch) {
				outPtrs[ch] = static_cast<float*>(jack_port_get_buffer(outPorts[ch], nframes));
			}

			for(uint32_t ch = 0; ch < config.inChannels; ++ch) {
				inPtrs[ch] = static_cast<float*>(jack_port_get_buffer(inPorts[ch], nframes));
			}

			Block block {
				.samplerate  = config.samplerate,
				.outChannels = config.outChannels,
				.inChannels  = config.inChannels,
				.frames      = static_cast<uint32_t>(nframes),
				.out         = config.outChannels > 0 ? outPtrs.data() : nullptr,
				.in          = config.inChannels > 0 ? inPtrs.data() : nullptr
			};

			if(callback) {
				callback(block);
			} else {
				for(uint32_t ch = 0; ch < block.outChannels; ++ch) {
					std::memset(block.out[ch], 0, block.frames * sizeof(float));
				}
			}

			return 0;
		}

		static void onShutdown(void* arg) {
			auto* self = static_cast<JACK*>(arg);
			self->running.store(false, std::memory_order_release);
		}

		jack_client_t* client = nullptr;
		std::vector<jack_port_t*> outPorts;
		std::vector<jack_port_t*> inPorts;
		std::vector<float*> outPtrs;
		std::vector<float*> inPtrs;
	};
}
