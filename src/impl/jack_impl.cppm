module;

#include <jack/jack.h>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

export module audio.jack;
export import audio.block;
export import audio.config;
export import audio.error;

import audio.abstract_core;

namespace mka::audio {

	int processCallback(jack_nframes_t nframes, void* arg);

	struct JackChannelHandle {
		Channel			config;	
		jack_port_t*	port	= nullptr;
	};
		
	export class JACK final: public AbstractCoreAudio {

	public:

		JACK() {
			jack_status_t status;
			client = jack_client_open("mka_audio_client", JackNoStartServer, &status);

			if(!client) return;

			jack_set_process_callback(client, processCallback, this);
	
			openedChannels.reserve(16);
		}

		~JACK() override {
			close();
		}

		std::vector<Channel> getChannels() {
			if (!client) return {};
		
			const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical);
			if (!ports) return {};	

			std::vector<Channel> channels;
			channels.reserve(16);

			const auto samplerate = jack_get_sample_rate(client);
			const auto bufferSize = jack_get_buffer_size(client);
			
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
					samplerate,
					bufferSize,
					mka::audio::Format::Float32,			
					isInput
				);
			}

			jack_free(ports);

			std::ranges::sort(channels, {}, &Channel::deviceName);

			return channels;
		}

		Result open(const Channel& channel) override {
			if(!client || running) return mka::audio::Fail; 
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
			if(!client) return;
			if(running) return;

			if(jack_activate(client) == 0) {
				running = true; 
				currentSampleRate = jack_get_sample_rate(client);
			}

		}

		void stop() override {
			if (client) {
				if (running.exchange(false)) {
					jack_deactivate(client);
				}
			}		
		}

		Result close() override {
			stop();
			if (client) {
				for (auto& h : openedChannels) {
					jack_port_unregister(client, h.port);
				}
				openedChannels.clear();
				jack_client_close(client);
				client = nullptr;
			}
			return mka::audio::Ok;
		}

	protected:
		void run() override {}

	private:

		friend int processCallback(jack_nframes_t nframes, void* arg);

		jack_client_t* client;
		std::vector<JackChannelHandle> openedChannels;
		
		size_t inputCounter = 0;
		size_t outputCounter = 0;
		size_t currentSampleRate = 0;
	};

	int processCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		
		ChannelInfo channelInfo {};
		channelInfo.frameCount = nframes;
		channelInfo.sampleRate = engine->currentSampleRate;
		
		for(auto& ch : engine->openedChannels) {
			auto buf = static_cast<float*>(jack_port_get_buffer(ch.port, nframes));
			if(ch.config.input) {
				if(channelInfo.input.channelCount < MAX_CHANNEL_COUNT) {
					channelInfo.input.data[channelInfo.input.channelCount++] = buf;
				}
			} else {
				if(channelInfo.output.channelCount < MAX_CHANNEL_COUNT) {
					channelInfo.output.data[channelInfo.output.channelCount++] = buf;
				}
			}
		}
		jack_status_t status;
		if(engine->callback) engine->callback(channelInfo);
		
		return 0;
	}
}

