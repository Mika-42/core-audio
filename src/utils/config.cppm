module;

#include <string>
#include <cstdint>
#include <vector>
#include <cstring>
#include <algorithm>

export module audio.config;
import audio.ring;

export namespace mka::audio {

	enum class ResampleQuality : uint8_t {
		RealtimeLowCpu,
		RealtimeHighQuality,
		OfflineBest
	};

	struct ResamplerState {
		// Ratio expressed as input frames consumed for one output frame produced.
		// - > 1.0: downsampling
		// - < 1.0: upsampling
		double step = 1.0;
		double phase = 0.0;
		ResampleQuality quality = ResampleQuality::RealtimeHighQuality;

		// Bootstrap cache keeps the first frames until we have enough context for
		// interpolation (especially for cubic mode that benefits from 4 points).
		float bootstrap[4] {};
		size_t bootstrapCount = 0;

		// Window used by interpolation kernels.
		// s0 s1 s2 s3 are consecutive input frames around the interpolation cursor.
		float s0 = 0.0f;
		float s1 = 0.0f;
		float s2 = 0.0f;
		float s3 = 0.0f;
		bool seeded = false;

		void configure(uint32_t inputRate, uint32_t outputRate,
					   ResampleQuality selectedQuality = ResampleQuality::RealtimeHighQuality) {
			quality = selectedQuality;

			if (inputRate == 0 || outputRate == 0) {
				step = 1.0;
				reset();
				return;
			}

			step = static_cast<double>(inputRate) / static_cast<double>(outputRate);
			reset();
		}

		void reset() {
			phase = 0.0;
			bootstrapCount = 0;
			std::memset(bootstrap, 0, sizeof(bootstrap));
			s0 = 0.0f;
			s1 = 0.0f;
			s2 = 0.0f;
			s3 = 0.0f;
			seeded = false;
		}

		static float linear(float a, float b, float t) {
			return a + ((b - a) * t);
		}

		// Cubic Hermite interpolation with Catmull-Rom tangent estimation.
		// This gives better HF behaviour than linear for almost the same cost,
		// and remains deterministic / allocation-free for realtime use.
		static float cubicHermite(float y0, float y1, float y2, float y3, float t) {
			const float c0 = y1;
			const float c1 = 0.5f * (y2 - y0);
			const float c2 = y0 - (2.5f * y1) + (2.0f * y2) - (0.5f * y3);
			const float c3 = (0.5f * (y3 - y0)) + (1.5f * (y1 - y2));
			return (((c3 * t) + c2) * t + c1) * t + c0;
		}

		// Moves interpolation window by one input frame.
		bool advanceWindow(const float* input, size_t inputFrames, size_t& consumedFrames) {
			s0 = s1;
			s1 = s2;
			s2 = s3;

			if (consumedFrames < inputFrames) {
				s3 = input[consumedFrames++];
				return true;
			}

			// If we have no new frame available, duplicate the edge sample to avoid
			// invalid reads. This still produces a valid stream and avoids clicks,
			// though interpolation quality naturally degrades near starvation.
			s3 = s2;
			return false;
		}

		// Streaming sample-rate conversion.
		// - No allocations
		// - Preserves phase between calls
		// - Designed for small realtime chunks
		size_t process(const float* input, size_t inputFrames,
					   float* output, size_t outputFrames, size_t& consumedFrames) {
			consumedFrames = 0;
			size_t producedFrames = 0;

			if (!input || !output || inputFrames == 0 || outputFrames == 0) {
				return 0;
			}

			if (step == 1.0) {
				const size_t frames = (inputFrames < outputFrames) ? inputFrames : outputFrames;
				std::memcpy(output, input, frames * sizeof(float));
				consumedFrames = frames;
				return frames;
			}

			if (!seeded) {
				while (bootstrapCount < 4 && consumedFrames < inputFrames) {
					bootstrap[bootstrapCount++] = input[consumedFrames++];
				}

				if (bootstrapCount < 2) {
					return 0;
				}

				// Edge duplication lets us start with partial context and then naturally
				// converge to fully-populated cubic windows as frames keep arriving.
				s0 = bootstrap[0];
				s1 = bootstrap[0];
				s2 = bootstrap[(bootstrapCount > 1) ? 1 : 0];
				s3 = bootstrap[(bootstrapCount > 2) ? 2 : (bootstrapCount - 1)];

				// Feed extra bootstrap sample into s3 when available.
				if (bootstrapCount > 3) {
					s3 = bootstrap[3];
				}

				seeded = true;
				bootstrapCount = 0;
			}

			while (producedFrames < outputFrames) {
				const float t = static_cast<float>(phase);

				if (quality == ResampleQuality::RealtimeLowCpu) {
					output[producedFrames++] = linear(s1, s2, t);
				} else {
					// RealtimeHighQuality and OfflineBest currently share cubic kernel.
					// OfflineBest is reserved for a future polyphase/sinc backend.
					output[producedFrames++] = cubicHermite(s0, s1, s2, s3, t);
				}

				phase += step;

				while (phase >= 1.0) {
					phase -= 1.0;
					if (!advanceWindow(input, inputFrames, consumedFrames)) {
						// We reached the end of current input chunk.
						// Stop now to avoid reusing duplicated edge too long.
						return producedFrames;
					}
				}
			}

			return producedFrames;
		}
	};

	struct RuntimeStats {
		size_t xrunCount = 0;
		size_t underrunCount = 0;
		size_t outputMissingFrames = 0;
		uint32_t backendSampleRate = 0;
		uint32_t backendBufferSize = 0;
		size_t openedChannels = 0;
		size_t openedInputs = 0;
		size_t openedOutputs = 0;
	};

	enum class Backend : uint8_t {
		Alsa, Jack, PipeWire, PulseAudio,
		Wasapi, Asio, DirectSound, Wmme,
		CoreAudio
	};

	namespace supported {
		inline constexpr uint32_t SampleRates[] {
			44'100, 48'000, 88'200, 96'000, 176'400, 192'000
		};

		inline constexpr uint32_t bufferSizes[] {
			64, 128, 256, 512, 1'024, 2'048, 4'096, 8'192
		};

		#if defined(_WIN32)
			inline constexpr Backend backends[] { Backend::Wasapi, Backend::Asio, Backend::DirectSound, Backend::Wmme };

		#elif defined(__linux__)
			inline constexpr Backend backends[] { Backend::Alsa, Backend::Jack, Backend::PipeWire, Backend::PulseAudio };

		#elif defined(__APPLE__)
			inline constexpr Backend backends[] { Backend::CoreAudio };
		#endif
	}

	enum class Format { Int16, Int24, Int32, Float32, Float64 };
	enum class Direction { In, Out };

	struct ChannelInfo {
		char name[256];
		Direction direction;
	};

	struct DeviceInfo {
		uint32_t sampleRate;
		uint32_t bufferSize;
		Format format;
	};

	struct Channel {
		DeviceInfo deviceInfo {};
		ChannelInfo channelInfo {};
		RingBuffer<float, constants::MAX_FIFO_SIZE> fifo {};
		ResamplerState inputResampler {};
		ResamplerState outputResampler {};
		float scratchBuffer[constants::MAX_BLOCK_SIZE];
	};
}
