module;

#include <string>
#include <cstdint>
#include <vector>

export module audio.config;
import audio.ring;

export namespace mka::audio {
	
	namespace supported {
		inline constexpr uint32_t SampleRates[] {
			44'100, 48'000, 88'200, 96'000, 176'400, 192'000
		};

		inline constexpr uint32_t bufferSizes[] {
			64, 128, 256, 512, 1'024, 2'048, 4'096, 8'192
		};
	}

	enum class Format { Int16, Int24, Int32, Float32, Float64 };

	enum class Direction { In, Out };

	struct ChannelInfo {	
		char name[256];
		Direction direction;
	};

	struct DeviceInfo {	
		uint32_t sampleRate;
	    uint32_t bufferSize;
	    Format format;
	};

	struct Channel {
		DeviceInfo deviceInfo {};
		ChannelInfo channelInfo {};
		RingBuffer<float, constants::MAX_FIFO_SIZE> fifo {};
		float scratchBuffer[constants::MAX_BLOCK_SIZE];
	};
}
