module;

#include <jack/jack.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

export module audio.jack;
export import audio.block;
export import audio.config;
export import audio.error;

import audio.abstract_core;

namespace mka::audio {
	
	// JACK callbacks
	int sampleRateCallback(jack_nframes_t nframes, void* arg);
	int bufferSizeCallback(jack_nframes_t nframes, void* arg);
	int processCallback(jack_nframes_t nframes, void* arg);
	int xrunCallback(void* arg);
	void shutdownCallback(void* arg);

	struct JackChannelHandle {
		Channel			config;
		jack_port_t*	port = nullptr;
	};
		
	export class JACK final : public AbstractCoreAudio {

	public:

		JACK() {
			jack_status_t status {};
			client = jack_client_open("mka_audio_client", JackNoStartServer, &status);

			if(!client) {
				state.store(State::Closed);
				return;
			}

			// setting callbacks
			jack_on_shutdown(client, shutdownCallback, this);
			jack_set_xrun_callback(client, xrunCallback, this);
			jack_set_process_callback(client, processCallback, this);
			jack_set_sample_rate_callback(client, sampleRateCallback, this);
			jack_set_buffer_size_callback(client, bufferSizeCallback, this);

			// setting JACK API value
			const uint32_t _jackSampleRate = jack_get_sample_rate(client);
			const uint32_t _jackBufferSize = jack_get_buffer_size(client);
	
			// setting audio engine value
			jackSampleRate.store(_jackSampleRate);
			jackBufferSize.store(_jackBufferSize);
			sampleRate.store(_jackSampleRate);
			blockSize.store(_jackBufferSize);
			
			state.store(State::Stopped);
		}

		~JACK() override {
			close();
		}

		std::vector<Channel> getChannels() override {
			if (!client) return {};
		
			const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical);
			if (!ports) return {};

			std::vector<Channel> channels;
			channels.reserve(16);
	
			for (int i = 0; ports[i]; ++i) {

				jack_port_t* port = jack_port_by_name(client, ports[i]);
				if (!port) continue;

				std::string_view fullName = ports[i];
				auto pos = fullName.find(':');
				if (pos == std::string_view::npos) continue;

		        std::string_view deviceName  = fullName.substr(0, pos);
		        std::string_view channelName = fullName.substr(pos + 1);
				const unsigned long flags = jack_port_flags(port);
				const bool isInput = (flags & JackPortIsOutput);

				channels.emplace_back(
					std::string(channelName),
					std::string(deviceName), 
					std::string(fullName),
					jackSampleRate.load(),
					jackBufferSize.load(),
					mka::audio::Format::Float32,			
					isInput
				);
			}

			jack_free(ports);

			std::ranges::sort(channels, {}, &Channel::deviceName);

			return channels;
		}

		Result open(const Channel& channel) override {
			
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if(!client) {
				return Result { Error::DeviceOpenFailed, "JACK client unavailable." };
			}
			
			if(state.load() != State::Stopped) {
				return Result { Error::WouldBlock, "Engine must be stopped before opening a channel." };
			}

			if (openedChannels.size() >= MAX_CHANNEL_COUNT) {
				return Result { Error::GenericError, "Maximum channel count reached." };
			}

			std::string name = channel.input ? "input_" : "output_";
			name += std::to_string(channel.input ? inputCounter++ : outputCounter++);

			jack_port_t* port = jack_port_register(
				client, 
				name.c_str(), 
				JACK_DEFAULT_AUDIO_TYPE, 
				channel.input ? JackPortIsInput : JackPortIsOutput,
				0
			);

			if(!port) {
				return Result { Error::DeviceOpenFailed, "Failed to register JACK port." };
			}

			openedChannels.emplace_back(channel, port);
			
			const int err = channel.input 
				? jack_connect(client, channel.port.c_str(), jack_port_name(port))
				: jack_connect(client, jack_port_name(port), channel.port.c_str());	
			
			if (err != 0) {
				jack_port_unregister(client, port);
				openedChannels.pop_back();
				return Result { Error::DeviceOpenFailed, "Failed to connect JACK port." };
			}
			
			return mka::audio::Ok;
		}

		void start() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (!client || state.load() != State::Stopped) return;

			state.store(State::Starting);

			if (jack_activate(client) == 0) {
				state.store(State::Running);
				return;
			}

			state.store(State::Stopped);
		}

		void stop() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();
		}

		Result close() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();

			if (client) {
				for (auto& h : openedChannels) {
					jack_port_unregister(client, h.port);
				}
				
				openedChannels.clear();
				jack_client_close(client);
				client = nullptr;
				state.store(State::Closed);
			}
			return mka::audio::Ok;
		}

	protected:
		void run() override {}

	private:
		
		void stopNoLock() {
			if (!client || state.load() != State::Running) return;
			state.store(State::Stopping);
			jack_deactivate(client);
			state.store(State::Stopped);
		}

	private:
		friend int bufferSizeCallback(jack_nframes_t nframes, void* arg);
		friend int sampleRateCallback(jack_nframes_t nframes, void* arg);
		friend int processCallback(jack_nframes_t nframes, void* arg);
		friend int xrunCallback(void* arg);
		friend void shutdownCallback(void* arg);

		jack_client_t* client = nullptr;
		std::vector<JackChannelHandle> openedChannels;
		
		size_t inputCounter = 0;
		size_t outputCounter = 0;

		std::atomic<uint32_t> jackSampleRate = 0;
		std::atomic<uint32_t> jackBufferSize = 0;
		std::atomic<uint64_t> xrunCount		 = 0;
		std::atomic<uint64_t> underrunCount	 = 0;
	};

	int sampleRateCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->jackSampleRate.store(nframes);
		return 0;
	}

	int bufferSizeCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->jackBufferSize.store(nframes);
		return 0;
	}

	void shutdownCallback(void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->state.store(State::Stopped);
	}

	int xrunCallback(void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->xrunCount.fetch_add(1);
		return 0;
	}

	int processCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
	
		const uint32_t jackRate		= engine->jackSampleRate.load();
		const uint32_t engineRate	= engine->sampleRate.load();
		const uint32_t blockSize	= engine->blockSize.load();
		const bool needResamle		= (jackRate != engineRate);
	
		if(jackRate == 0 || engineRate == 0) return 0;

		//----
		Block block {};
		info.frameCount = nframes;
		info.sampleRate = engine->sampleRate.load();
		auto& ch = engine->openedChannels;

		for(size_t c = 0; c < ch.size(); ++c) {
			float* buf = static_cast<float*>(jack_port_get_buffer(ch[c].port, nframes));
			if(!buf) continue;
			
			if(block.inputCount + block.outputCount >= MAX_CHANNEL_COUNT) {
				// fail : too many channels
				return;
			}

			if(engine->openedChannels.config.input) {
					block.inputs[block.inputCount++] = buf;
			} else {
					block.outputs[block.outputCount++] = buf;
			}
		
		}
		
		uint32_t sr = ch.config.sampleRate.value_or(jackRate);

			if(sr != jackRate) {
				//in resampling
			}

		if(engine->callback) {
			engine->callback(info);
		} else {
			for (size_t ch = 0; ch < info.output.channelCount; ++ch) {
				std::memset(info.output.data[ch], 0, sizeof(float) * nframes);
			}
		}
		
		if(sr != jackRate) {
				//out resampling
		}
		return 0;
	}
}
	
