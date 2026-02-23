module;
#include <cstddef>
export module audio.constants;

export namespace mka::audio::constants {
	inline constexpr size_t MAX_AUDIO_BLOCKS = 16; //2^4
	inline constexpr size_t MAX_FRAMES_COUNT = 4096; //2^12
	inline constexpr size_t MAX_FIFO_SIZE = MAX_FRAMES_COUNT * MAX_AUDIO_BLOCKS; //2^16
}
