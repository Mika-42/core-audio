module;

#include <algorithm>
#include <cstdint>
#include <cstring>

export module audio.midi_timeline;

export import audio.midi_block;
import audio.constants;

export namespace mka::audio::midi {

	struct PendingEvent {
		uint64_t absoluteFrame = 0;
		uint8_t size = 0;
		uint8_t data[constants::MAX_MIDI_EVENT_DATA_SIZE] {};
	};

	// Fixed-size ring queue (no dynamic allocation) dedicated to realtime usage.
	struct Queue {
		PendingEvent events[constants::MAX_PENDING_MIDI_EVENTS] {};
		size_t head = 0;
		size_t count = 0;
	};

	// Maintains a monotonic timeline in engine frames while backend callbacks
	// progress in backend frames.
	struct FrameAlignState {
		uint64_t absoluteEngineFrame = 0;
		uint64_t remainder = 0;

		void reset() {
			absoluteEngineFrame = 0;
			remainder = 0;
		}
	};

	inline uint64_t backendFrameToEngineFrame(uint64_t backendFrame, uint32_t backendRate, uint32_t engineRate) {
		if (backendRate == 0) {
			return 0;
		}

		return (backendFrame * static_cast<uint64_t>(engineRate)) / static_cast<uint64_t>(backendRate);
	}

	inline void advanceEngineTimeline(FrameAlignState& timeline,
								  uint64_t backendFrames,
								  uint32_t backendRate,
								  uint32_t engineRate) {
		if (backendRate == 0) {
			return;
		}

		// Fixed-point accumulation avoids floating point drift in realtime callback.
		timeline.remainder += backendFrames * static_cast<uint64_t>(engineRate);
		timeline.absoluteEngineFrame += timeline.remainder / static_cast<uint64_t>(backendRate);
		timeline.remainder = timeline.remainder % static_cast<uint64_t>(backendRate);
	}

	inline bool pushEvent(Queue& queue, uint64_t absoluteFrame, const uint8_t* data, size_t size) {
		if (!data || size == 0 || queue.count >= constants::MAX_PENDING_MIDI_EVENTS) {
			return false;
		}

		const size_t slot = (queue.head + queue.count) % constants::MAX_PENDING_MIDI_EVENTS;
		auto& dst = queue.events[slot];
		dst.absoluteFrame = absoluteFrame;
		dst.size = static_cast<uint8_t>(std::min(size, constants::MAX_MIDI_EVENT_DATA_SIZE));
		std::memcpy(dst.data, data, dst.size);
		queue.count++;
		return true;
	}

	inline size_t collectEventsForBlock(Queue& queue,
							 uint64_t blockStartFrame,
							 uint32_t blockSize,
							 mka::midi::EventView* destination,
							 size_t destinationCapacity) {
		if (!destination || destinationCapacity == 0 || blockSize == 0) {
			return 0;
		}

		const uint64_t blockEndFrame = blockStartFrame + static_cast<uint64_t>(blockSize);
		size_t written = 0;

		while (queue.count > 0) {
			auto& event = queue.events[queue.head];

			// If callback timing jumps forward, stale events are dropped to keep
			// deterministic alignment and bounded processing cost.
			if (event.absoluteFrame < blockStartFrame) {
				queue.head = (queue.head + 1) % constants::MAX_PENDING_MIDI_EVENTS;
				queue.count--;
				continue;
			}

			if (event.absoluteFrame >= blockEndFrame || written >= destinationCapacity) {
				break;
			}

			destination[written].frameOffset = static_cast<uint32_t>(event.absoluteFrame - blockStartFrame);
			destination[written].size = event.size;
			destination[written].data = event.data;
			written++;

			queue.head = (queue.head + 1) % constants::MAX_PENDING_MIDI_EVENTS;
			queue.count--;
		}

		return written;
	}
}
