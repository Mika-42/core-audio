module;

#include <cstdint>
#include <cstddef>

export module audio.block;

export namespace mka::audio {
	
	constexpr size_t MAX_CHANNEL_COUNT	= 16;	//2^4
	constexpr size_t MAX_FRAMES_COUNT	= 4096;	//2^12

	struct Block {	
		uint32_t	blockSize = 0;
		uint32_t	sampleRate = 0;
		uint32_t	inputCount = 0;	
		uint32_t	outputCount = 0;	
		float		inputs[MAX_CHANNEL_COUNT][MAX_FRAMES_COUNT];
		float		outputs[MAX_CHANNEL_COUNT][MAX_FRAMES_COUNT];
	};

	typedef void(*Callback)(Block&);
}
