module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <atomic>

export module audio.midi;

export import audio.constants;
import audio.ring;

export namespace mka::audio {

	// Représente un événement MIDI dans la timeline absolue du moteur
	// (timestamp en samples moteur).
	struct MidiEvent {
		uint64_t sampleTime = 0;
		uint8_t size = 0;
		std::array<uint8_t, constants::MAX_MIDI_MESSAGE_SIZE> data {};
	};

	// Représente un événement MIDI local à un bloc moteur
	// (frameOffset = index sample dans le bloc courant).
	struct MidiBlockEvent {
		uint32_t frameOffset = 0;
		uint8_t size = 0;
		std::array<uint8_t, constants::MAX_MIDI_MESSAGE_SIZE> data {};
	};

	// Vue non-allocante transmise au callback audio pour un bloc donné.
	struct MidiBlockView {
		uint64_t startSample = 0;
		const MidiBlockEvent* events = nullptr;
		uint32_t eventCount = 0;
	};

	// Queue SPSC minimale, basée sur RingBuffer existant.
	template<size_t N = constants::MAX_MIDI_QUEUE_SIZE>
	class MidiEventQueue {
	public:
		bool push(const MidiEvent& event) noexcept {
			return fifo.push(&event, 1) == 1;
		}

		// Extrait les événements appartenant à la fenêtre [blockStartSample, blockEndSample)
		// et les convertit en offsets locaux du bloc.
		size_t popForBlock(uint64_t blockStartSample,
							 uint32_t blockSize,
							 MidiBlockEvent* outEvents,
							 size_t outCapacity) noexcept {
			if (!outEvents || outCapacity == 0 || blockSize == 0) {
				return 0;
			}

			const uint64_t blockEndSample = blockStartSample + static_cast<uint64_t>(blockSize);
			size_t written = 0;

			while (written < outCapacity) {
				if (!pendingEventValid) {
					if (fifo.pop(&pendingEvent, 1) != 1) {
						break;
					}
					pendingEventValid = true;
				}

				if (pendingEvent.sampleTime >= blockEndSample) {
					break;
				}

				uint64_t clampedSample = pendingEvent.sampleTime;
				if (clampedSample < blockStartSample) {
					// En cas d'événement en retard, on le cale sur le sample 0 du bloc.
					clampedSample = blockStartSample;
				}

				MidiBlockEvent blockEvent {};
				blockEvent.frameOffset = static_cast<uint32_t>(clampedSample - blockStartSample);
				blockEvent.size = pendingEvent.size;
				blockEvent.data = pendingEvent.data;
				outEvents[written++] = blockEvent;

				pendingEventValid = false;
			}

			return written;
		}

		// Droppe les événements supplémentaires de la fenêtre courante.
		// Cette stratégie évite de reporter ces événements au bloc suivant,
		// ce qui créerait une distorsion temporelle audible.
		size_t discardOverflowForBlock(uint64_t blockStartSample, uint32_t blockSize) noexcept {
			if (blockSize == 0) {
				return 0;
			}

			const uint64_t blockEndSample = blockStartSample + static_cast<uint64_t>(blockSize);
			size_t dropped = 0;

			while (true) {
				if (!pendingEventValid) {
					if (fifo.pop(&pendingEvent, 1) != 1) {
						break;
					}
					pendingEventValid = true;
				}

				if (pendingEvent.sampleTime >= blockEndSample) {
					break;
				}

				pendingEventValid = false;
				++dropped;
			}

			return dropped;
		}

		void reset() noexcept {
			MidiEvent tmp {};
			while (fifo.pop(&tmp, 1) == 1) {}
			pendingEventValid = false;
			pendingEvent = MidiEvent {};
		}

	private:
		RingBuffer<MidiEvent, N> fifo {};
		MidiEvent pendingEvent {};
		bool pendingEventValid = false;
	};

	// Composant haut niveau qui simplifie le flux:
	// - ingest d'événements backend
	// - timeline sample absolue
	// - extraction directe par bloc moteur
	class MidiTimeline {
	public:
		void reset(uint64_t startSample = 0) noexcept {
			sampleCursor = startSample;
			queue.reset();
			queueOverflowCount.store(0, std::memory_order_relaxed);
			blockOverflowCount.store(0, std::memory_order_relaxed);
		}

		uint64_t currentSample() const noexcept {
			return sampleCursor;
		}

		void advance(uint32_t frames) noexcept {
			sampleCursor += frames;
		}

		bool pushEvent(const MidiEvent& event) noexcept {
			const bool pushed = queue.push(event);
			if (!pushed) {
				queueOverflowCount.fetch_add(1, std::memory_order_relaxed);
			}
			return pushed;
		}

		// Convertit un offset backend -> offset moteur, puis push en timeline absolue.
		bool pushBackendOffsetEvent(uint64_t callbackStartSample,
								   uint32_t backendOffsetFrames,
								   uint32_t engineSampleRate,
								   uint32_t backendSampleRate,
								   const uint8_t* bytes,
								   size_t size) noexcept {
			if (!bytes || size == 0 || backendSampleRate == 0) {
				return false;
			}

			MidiEvent event {};
			event.size = static_cast<uint8_t>(std::min(size, static_cast<size_t>(constants::MAX_MIDI_MESSAGE_SIZE)));
			for (uint8_t i = 0; i < event.size; ++i) {
				event.data[i] = bytes[i];
			}

			// Conversion déterministe en samples moteur (entière, sans alloc).
			const uint64_t engineOffsetSamples =
				(static_cast<uint64_t>(backendOffsetFrames) * static_cast<uint64_t>(engineSampleRate))
				/ static_cast<uint64_t>(backendSampleRate);

			event.sampleTime = callbackStartSample + engineOffsetSamples;

			const bool pushed = queue.push(event);
			if (!pushed) {
				queueOverflowCount.fetch_add(1, std::memory_order_relaxed);
			}
			return pushed;
		}

		MidiBlockView prepareBlock(uint32_t blockSize) noexcept {
			MidiBlockView view {};
			view.startSample = sampleCursor;

			const size_t count = queue.popForBlock(
				sampleCursor,
				blockSize,
				blockEvents.data(),
				blockEvents.size()
			);

			// Si le nombre d'événements MIDI dans cette fenêtre dépasse la capacité
			// exportée au callback, le surplus est explicitement jeté dans la même
			// fenêtre temporelle. On évite ainsi de décaler des événements sur le
			// bloc suivant (distorsion temporelle perçue).
			const size_t droppedInBlock = queue.discardOverflowForBlock(
				sampleCursor,
				blockSize
			);

			blockOverflowCount.fetch_add(droppedInBlock, std::memory_order_relaxed);

			view.events = blockEvents.data();
			view.eventCount = static_cast<uint32_t>(count);
			return view;
		}

		size_t queueOverflows() const noexcept {
			return queueOverflowCount.load(std::memory_order_relaxed);
		}

		size_t blockOverflows() const noexcept {
			return blockOverflowCount.load(std::memory_order_relaxed);
		}

	private:
		MidiEventQueue<> queue {};
		std::array<MidiBlockEvent, constants::MAX_MIDI_EVENTS_PER_BLOCK> blockEvents {};
		uint64_t sampleCursor = 0;
		std::atomic<size_t> queueOverflowCount = 0;
		std::atomic<size_t> blockOverflowCount = 0;
	};
}

