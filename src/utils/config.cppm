module;

#include <string>
#include <cstdint>
#include <vector>
#include <cstring>
#include <algorithm>

export module audio.config;

export namespace mka::audio {

	/*namespace supported {
		inline constexpr uint32_t SampleRates[] {
			44'100, 48'000, 88'200, 96'000, 176'400, 192'000
		};

		inline constexpr uint32_t bufferSizes[] {
			64, 128, 256, 512, 1'024, 2'048, 4'096, 8'192
		};

		#if defined(_WIN32)
			inline constexpr Backend backends[] { Backend::Wasapi, Backend::Asio, Backend::DirectSound, Backend::Wmme };

		#elif defined(__linux__)
			inline constexpr Backend backends[] { Backend::Alsa, Backend::Jack, Backend::PipeWire, Backend::PulseAudio };

		#elif defined(__APPLE__)
			inline constexpr Backend backends[] { Backend::CoreAudio };
		#endif
	}*/
}
