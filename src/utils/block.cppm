module;

#include <cstdint>
#include <cstddef>

export module audio.block;

export namespace mka::audio {
	
//	constexpr size_t MAX_CHANNEL_COUNT = 64;

	struct Block {	
		float**	data = nullptr;
		uint32_t channelCount = 0;
		uint32_t frameCount = 0;

		float* operator[](uint32_t ch) const {
			return data[ch];
		}
//		float*	data[MAX_CHANNEL_COUNT] = {};
	};

	struct ChannelInfo {
		uint32_t	sampleRate = 0;
		uint32_t	blockSize = 0;
	
		Block		input;	
		Block		output;	
	};

	typedef void(*Callback)(const ChannelInfo&);
}
