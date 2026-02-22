module;

#include <cstdint>
#include <cstddef>

export module audio.block;

export namespace mka::audio {
	
	constexpr size_t MAX_CHANNEL_COUNT = 64;

	struct Block {	
		size_t	channelCount = 0;	
		float*	data[MAX_CHANNEL_COUNT] = {};
	};

	struct ChannelInfo {
		Block	input;	
		Block	output;	
		size_t	sampleRate = 0;
		size_t	frameCount = 0;
	};

	typedef void(*Callback)(const ChannelInfo&);
}
