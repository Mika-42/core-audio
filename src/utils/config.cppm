channelInfo.input.channelCountmodule;

#include <string>
#include <cstdint>
#include <vector>
#include <optional>

export module audio.config;

export namespace mka::audio {
	
	namespace supported {
		inline constexpr uint32_t SampleRates[] {
			44'100, 48'000, 88'200, 96'000, 176'400, 192'000
		};

		inline constexpr uint32_t bufferSizes[] {
			16, 32, 64, 128, 256, 512, 1'024, 2'048, 4'096, 8'192
		};
	}

	enum class Format {

		Int16,
		Int24,
		Int32,
		Float32,
		Float64,

	};

	struct Channel {
		std::string	name;
		std::string	deviceName;
		std::string	port;

		std::optional<uint32_t> sampleRate;
	    std::optional<uint32_t> bufferSize;
	    std::optional<Format> format;

		bool input;
	};
}
