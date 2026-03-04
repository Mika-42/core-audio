module;

#include <cstdint>

export module audio.midi_block;

export namespace mka::midi {
	struct EventView {
		uint32_t frameOffset = 0;
		uint8_t size = 0;
		const uint8_t* data = nullptr;
	};

	struct Block {
		uint32_t eventCount = 0;
		const EventView* events = nullptr;
	};
}
