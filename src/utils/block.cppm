module;

#include <cstdint>
#include <cstddef>

export module audio.block;
import audio.constants;

export namespace mka::audio {
	constexpr uint32_t MAX_MIDI_EVENT_SIZE = 16;
	constexpr uint32_t MAX_MIDI_EVENTS_PER_BLOCK = 256;

	struct MidiEvent {
		uint32_t frameOffset = 0;
		uint32_t source = 0;
		uint8_t size = 0;
		bool truncated = false;
		uint8_t data[MAX_MIDI_EVENT_SIZE] {};
	};

	struct MidiEventBlock {
		uint32_t frameCount = 0;
		uint32_t eventCount = 0;
		const MidiEvent* events = nullptr;
	};

	struct Block {	
		uint32_t	blockSize = 0;
		uint32_t	sampleRate = 0;
		uint32_t	inputCount = 0;	
		uint32_t	outputCount = 0;	
		float*		inputs [constants::MAX_CHANNEL_COUNT] {};
		float*		outputs[constants::MAX_CHANNEL_COUNT] {};
	};

	typedef void(*Callback)(Block&, const MidiEventBlock&);
}
