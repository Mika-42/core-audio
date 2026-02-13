module;

#include <string>
#include <cstdint>

export module audio.config;

export namespace mka::audio {
	
	enum class Format {
		Int16,
		Int24,
		Int32,
		Float32,
		Float64,
	};

	struct Config {
		uint32_t	samplerate;
		uint32_t	bufferSize;
		uint32_t	outChannels;
		uint32_t	inChannels;
		Format		audioFormat;

		std::string name;
	};
}
