module;

#include <string>
#include <cstdint>

export module audio.config;

export namespace mka::audio {
	struct Config {
		uint32_t	samplerate;
		uint32_t	bufferSize;
		uint32_t	channels;
		std::string name;
	};
}
