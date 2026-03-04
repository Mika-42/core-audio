module;

#include <cstdint>
#include <cstddef>

export module audio.block;
import audio.constants;
export import audio.midi_block;

export namespace mka::audio {
	struct Block {	
		uint32_t	blockSize = 0;
		uint32_t	sampleRate = 0;
		uint32_t	inputCount = 0;	
		uint32_t	outputCount = 0;	
		float*		inputs [constants::MAX_CHANNEL_COUNT] {};
		float*		outputs[constants::MAX_CHANNEL_COUNT] {};
	};

	typedef void(*Callback)(Block&, const mka::midi::Block&);
}
