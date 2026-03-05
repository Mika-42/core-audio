module;

#include <cstdint>
#include <cstddef>

export module audio.block;
import audio.constants;
import audio.midi;

export namespace mka::audio {
	struct Block {	
		uint32_t	blockSize = 0;
		uint32_t	sampleRate = 0;
		uint64_t	startSample = 0;
		uint32_t	inputCount = 0;	
		uint32_t	outputCount = 0;	
		uint32_t	midiEventCount = 0;
		const MidiBlockEvent* midiEvents = nullptr;
		float*		inputs [constants::MAX_CHANNEL_COUNT] {};
		float*		outputs[constants::MAX_CHANNEL_COUNT] {};	
	};

	typedef void(*Callback)(Block&);
}
