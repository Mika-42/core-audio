module;

#include <atomic>
#include <cstring>
#include <cstddef>
#include <concepts>
#include <type_traits>

export module audio.ring;
export import audio.constants;

namespace mka::audio {

	
	static_assert((constants::MAX_FIFO_SIZE & (constants::MAX_FIFO_SIZE - 1)) == 0, "MAX_FIFO_SIZE must be power of two");

	template<typename T> concept TrivialCopyable = std::is_trivially_copyable_v<T>;

	export template<TrivialCopyable T, size_t N> class RingBuffer {
		static_assert((N & (N - 1)) == 0, "N must be power of two");
		public:

			size_t available() const noexcept {
				const size_t w = wIndex.load(std::memory_order_acquire);
				const size_t r = rIndex.load(std::memory_order_acquire);
				return w - r;
			}

			size_t push(const T* src, size_t len) noexcept {
	
				const size_t w = wIndex.load(std::memory_order_relaxed);
	            const size_t r = rIndex.load(std::memory_order_acquire);

				const size_t freeSpace = N - (w - r);
				if (len > freeSpace) len = freeSpace;
				if (len == 0) return 0;

				const size_t wPos = w & mask;
				const size_t spaceUntilEnd = N - wPos;
				const size_t part1 = (len < spaceUntilEnd) ? len : spaceUntilEnd;

				// Copy part 1
				std::memcpy(buffer + wPos, src, part1 * sizeof(T));

				// Copy part 2
				if (len > part1) {
					std::memcpy(buffer, src + part1, (len - part1) * sizeof(T));
				}
				
				wIndex.store(w + len, std::memory_order_release);
				return len;
			}

			size_t pop(T* dst, size_t len) noexcept {

				const size_t r = rIndex.load(std::memory_order_relaxed);
	            const size_t w = wIndex.load(std::memory_order_acquire);

			    const size_t availableSamples = w - r;
				if (len > availableSamples) len = availableSamples;
				if (len == 0) return 0;

				const size_t rPos = r & mask;

				const size_t spaceUntilEnd = N - rPos;
				const size_t part1 = (len < spaceUntilEnd) ? len : spaceUntilEnd;

				// Copy part 1
				std::memcpy(dst, buffer + rPos, part1 * sizeof(T));

				// Copy part 2
				if (len > part1) {
					std::memcpy(dst + part1, buffer, (len - part1) * sizeof(T));
				}

				rIndex.store(r + len, std::memory_order_release);
				return len;
			}

		private:
			static constexpr size_t mask = N - 1;
			alignas(64) T buffer[N];
			alignas(64) std::atomic<size_t> rIndex {0};
			alignas(64) std::atomic<size_t> wIndex {0};
	}
}
