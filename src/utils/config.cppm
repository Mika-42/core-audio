module;

#include <string>
#include <cstdint>
#include <vector>

export module audio.config;
import audio.ring;

export namespace mka::audio {
	
	enum class Backend : uint8_t { 
		Alsa, Jack, PipeWire, PulseAudio,
		Wasapi, Asio, DirectSound, Wmme,
		CoreAudio						  
	};

	namespace supported {
		inline constexpr uint32_t SampleRates[] {
			44'100, 48'000, 88'200, 96'000, 176'400, 192'000
		};

		inline constexpr uint32_t bufferSizes[] {
			64, 128, 256, 512, 1'024, 2'048, 4'096, 8'192
		};
	
		#if defined(_WIN32)
			inline constexpr Backend backends[] { Wasapi, Asio, DirectSound, Wmme };

		#elif defined(__linux__)
			inline constexpr Backend backends[] { Alsa, Jack, PipeWire, PulseAudio };

		#elif defined(__APPLE__)
			inline constexpr Backend backends[] { CoreAudio };
		#endif
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
