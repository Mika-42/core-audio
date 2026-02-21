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

export namespace mka::audio {
	
	class JACK final: public AbstractCoreAudio {

	public:

		JACK() {
		    client = jack_client_open("mka_audio_client", JackNoStartServer, nullptr);
		}

		std::vector<Channel> getChannels() {
			if (!client) return {};
		
			const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical);
			if (!ports) return {};	

			std::vector<Channel> channels;
			
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
		
		}

		Result close() override {
			if (client) {
				jack_client_close(client);
				client = nullptr;
			}
			return mka::audio::Ok;
		}

	protected:
		void run() override {}

	private:
		jack_client_t* client;
		std::vector<jack_port_t*> outPorts;
	};
}

