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
	
	// JACK Callbacks
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

			if(!client) return;

			// setting callbacks
			jack_on_shutdown(client, shutdownCallback, this);
			jack_set_xrun_callback(client, xrunCallback, this);	
			jack_set_process_callback(client, processCallback, this);
			jack_set_sample_rate_callback(client, sampleRateCallback, this);
			jack_set_buffer_size_callback(client, bufferSizeCallback, this);

			// setting JACK API value
			currentSampleRate.store(jack_get_sample_rate(client));
			currentBufferSize.store(jack_get_buffer_size(client));
			
			// setting audio engine value
			sampleRate.store(currentSampleRate.load());
			bufferSize.store(currentBufferSize.load());
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

				std::string_view full_name = ports[i];

				auto pos = full_name.find(':');
				if (pos == std::string_view::npos) continue;

		        std::string_view device_name  = full_name.substr(0, pos);
		        std::string_view channel_name = full_name.substr(pos + 1);

				unsigned long flags = jack_port_flags(port);
				const bool isInput = (flags & JackPortIsOutput);

				channels.emplace_back(
					std::string(channel_name),
					std::string(device_name), 
					std::string(full_name),
					currentSampleRate.load(),
					currentBufferSize.load(),
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
			if(!client) return mka::audio::Fail;
			if(state.load() != State::Stopped) return mka::audio::Fail;


			std::string name;

			if(channel.input) {
				name = "input_" + std::to_string(inputCounter++);
			} else {
				name = "output_" + std::to_string(outputCounter++);
			}

			jack_port_t* port = jack_port_register(
				client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, 
				channel.input ? JackPortIsInput : JackPortIsOutput, 0
			);

			if(!port) return mka::audio::Fail;

			openedChannels.emplace_back(channel, port);
			
			int err = 0;
			if(channel.input) {
				err = jack_connect(client, channel.port.c_str(), jack_port_name(port));
			} else {		
				err = jack_connect(client, jack_port_name(port), channel.port.c_str());	
			}

			if (err != 0) {
				jack_port_unregister(client, port);
				openedChannels.pop_back();
				return mka::audio::Fail;
			}
			
			return mka::audio::Ok;
		}

		void start() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if(!client) return;

			if(state.load() != State::Stopped) return;

			state.store(State::Starting);

			if(jack_activate(client) == 0) {
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
			if (!client) return;
			if (state.load() != State::Running) return;
			state.store(Stopping);
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

		std::atomic<uint32_t> currentSampleRate = 0;
		std::atomic<uint32_t> currentBufferSize = 0;
		std::atomic<uint32_t> xrunCount			= 0;
	};

	int sampleRateCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->currentSampleRate.store(nframes);
		return 0;
	}

	int bufferSizeCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->currentBufferSize.store(nframes);
		return 0;
	}

	void shutdownCallback(void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->running.store(false);
	}

	int xrunCallback(void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->xrunCount.fetch_add(1);
		return 0;
	}

	int processCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		
		ChannelInfo info {};
		info.frameCount = nframes;
		info.sampleRate = engine->sampleRate.load();
		
		for(auto& ch : engine->openedChannels) {
			auto buf = static_cast<float*>(jack_port_get_buffer(ch.port, nframes));
			if(!buf) continue;

			uint32_t sr = ch.config.sampleRate.value_or(engine->currentSampleRate);
		
			if(sr != engine->currentSampleRate.load()) {
				//resampling
			}

			if(ch.config.input) {
				if(info.input.channelCount < MAX_CHANNEL_COUNT) {
					info.input.data[info.input.channelCount++] = buf;
				}
			} else {
				if(info.output.channelCount < MAX_CHANNEL_COUNT) {
					info.output.data[info.output.channelCount++] = buf;
				}
			}
		}
		
		if(engine->callback) {
			engine->callback(info);
		} else {
			for (size_t ch = 0; ch < info.output.channelCount; ++ch) {
				std::memset(info.output.data[ch], 0, sizeof(float) * nframes);
			}
		}
		
		return 0;
	}
}
	
