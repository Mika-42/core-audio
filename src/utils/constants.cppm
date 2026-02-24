module;
#include <limits>
#include <cstddef>
export module audio.constants;

export namespace mka::audio::constants {
	inline constexpr size_t MAX_ITERATION		= std::numeric_limits<std::size_t>::max();
	inline constexpr size_t MAX_CHANNEL_COUNT	= 64;	//2^6
	inline constexpr size_t MAX_BLOCK_COUNT		= 16;	//2^4
	inline constexpr size_t MAX_BLOCK_SIZE		= 8192;	//2^13
	inline constexpr size_t MAX_FIFO_SIZE		= MAX_BLOCK_COUNT * MAX_BLOCK_SIZE; //2^17
}
