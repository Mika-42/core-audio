module;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

export module audio.pipewire;
export import audio.block;
export import audio.config;

import audio.error;
import audio.abstract_core;

namespace {
	enum class SampleEncoding {
		Int16,
		Int24In32,
		Int32,
		Float32,
		Float64
	};

	struct AudioFormatSpec {
		SampleEncoding encoding;
		spa_audio_format spa;
		uint32_t bytesPerSample;
	};

	AudioFormatSpec selectFormat(mka::audio::Format format) {
		switch(format) {
			case mka::audio::Format::Int16:   return { SampleEncoding::Int16,   SPA_AUDIO_FORMAT_S16P,    2 };
			case mka::audio::Format::Int24:   return { SampleEncoding::Int24In32, SPA_AUDIO_FORMAT_S24_32P, 4 };
			case mka::audio::Format::Int32:   return { SampleEncoding::Int32,   SPA_AUDIO_FORMAT_S32P,    4 };
			case mka::audio::Format::Float32: return { SampleEncoding::Float32, SPA_AUDIO_FORMAT_F32P,    4 };
			case mka::audio::Format::Float64: return { SampleEncoding::Float64, SPA_AUDIO_FORMAT_F64P,    8 };
		}
		return { SampleEncoding::Float32, SPA_AUDIO_FORMAT_F32P, 4 };
	}

	inline float clampUnit(float x) {
		return std::max(-1.0f, std::min(1.0f, x));
	}

	void decodeToFloat(const void* src, float* dst, uint32_t frames, const AudioFormatSpec& spec) {
		switch(spec.encoding) {
			case SampleEncoding::Int16: {
				auto* s = static_cast<const int16_t*>(src);
				for(uint32_t i = 0; i < frames; ++i) dst[i] = static_cast<float>(s[i]) / 32768.0f;
				break;
			}
			case SampleEncoding::Int24In32: {
				auto* s = static_cast<const int32_t*>(src);
				for(uint32_t i = 0; i < frames; ++i) dst[i] = static_cast<float>(s[i] >> 8) / 8388608.0f;
				break;
			}
			case SampleEncoding::Int32: {
				auto* s = static_cast<const int32_t*>(src);
				for(uint32_t i = 0; i < frames; ++i) dst[i] = static_cast<float>(s[i] / 2147483648.0);
				break;
			}
			case SampleEncoding::Float32: {
				std::memcpy(dst, src, frames * sizeof(float));
				break;
			}
			case SampleEncoding::Float64: {
				auto* s = static_cast<const double*>(src);
				for(uint32_t i = 0; i < frames; ++i) dst[i] = static_cast<float>(s[i]);
				break;
			}
		}
	}

	void encodeFromFloat(const float* src, void* dst, uint32_t frames, const AudioFormatSpec& spec) {
		switch(spec.encoding) {
			case SampleEncoding::Int16: {
				auto* d = static_cast<int16_t*>(dst);
				for(uint32_t i = 0; i < frames; ++i) {
					const float x = clampUnit(src[i]);
					d[i] = static_cast<int16_t>(std::lrintf(x * 32767.0f));
				}
				break;
			}
			case SampleEncoding::Int24In32: {
				auto* d = static_cast<int32_t*>(dst);
				for(uint32_t i = 0; i < frames; ++i) {
					const float x = clampUnit(src[i]);
					const int32_t s24 = static_cast<int32_t>(std::lrintf(x * 8388607.0f));
					d[i] = s24 << 8;
				}
				break;
			}
			case SampleEncoding::Int32: {
				auto* d = static_cast<int32_t*>(dst);
				for(uint32_t i = 0; i < frames; ++i) {
					const float x = clampUnit(src[i]);
					d[i] = static_cast<int32_t>(std::lrintf(x * 2147483647.0f));
				}
				break;
			}
			case SampleEncoding::Float32: {
				std::memcpy(dst, src, frames * sizeof(float));
				break;
			}
			case SampleEncoding::Float64: {
				auto* d = static_cast<double*>(dst);
				for(uint32_t i = 0; i < frames; ++i) d[i] = static_cast<double>(src[i]);
				break;
			}
		}
	}
}

export namespace mka::audio {

	class PipeWire final: public AbstractCoreAudio {
	public:
		PipeWire() = default;

		~PipeWire() override {
			close();
		}

		Result open(const Config& cfg) override {
			if(cfg.inChannels == 0 && cfg.outChannels == 0) {
				return Result{Error::SetupHardwareParameterFailed, "PipeWire backend requires at least one input or output channel"};
			}

			config = cfg;
			formatSpec = selectFormat(config.audioFormat);

			outScratch.assign(config.outChannels, std::vector<float>(config.bufferSize, 0.0f));
			inScratch.assign(config.inChannels, std::vector<float>(config.bufferSize, 0.0f));
			outPtrs.resize(config.outChannels, nullptr);
			inPtrs.resize(config.inChannels, nullptr);

			for(uint32_t ch = 0; ch < config.outChannels; ++ch) outPtrs[ch] = outScratch[ch].data();
			for(uint32_t ch = 0; ch < config.inChannels; ++ch) inPtrs[ch] = inScratch[ch].data();

			pw_init(nullptr, nullptr);

			loop = pw_thread_loop_new("mka-pipewire", nullptr);
			if(!loop) return Result{Error::DeviceOpenFailed, "Failed to create PipeWire thread loop"};

			context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
			if(!context) {
				close();
				return Result{Error::DeviceOpenFailed, "Failed to create PipeWire context"};
			}

			core = pw_context_connect(context, nullptr, 0);
			if(!core) {
				close();
				return Result{Error::DeviceOpenFailed, "Failed to connect PipeWire core"};
			}

			if(config.outChannels > 0) {
				playback = createStream("Playback", "Music");
				if(!playback) {
					close();
					return Result{Error::DeviceOpenFailed, "Failed to create PipeWire playback stream"};
				}

				playbackEvents = pw_stream_events{};
				playbackEvents.version = PW_VERSION_STREAM_EVENTS;
				playbackEvents.process = &PipeWire::onPlaybackProcess;
				pw_stream_add_listener(playback, &playbackListener, &playbackEvents, this);

				if(connectStream(playback, config.outChannels, PW_DIRECTION_OUTPUT) < 0) {
					close();
					return Result{Error::DeviceOpenFailed, "Failed to connect PipeWire playback stream"};
				}
			}

			if(config.inChannels > 0) {
				capture = createStream("Capture", "Production");
				if(!capture) {
					close();
					return Result{Error::DeviceOpenFailed, "Failed to create PipeWire capture stream"};
				}

				captureEvents = pw_stream_events{};
				captureEvents.version = PW_VERSION_STREAM_EVENTS;
				captureEvents.process = &PipeWire::onCaptureProcess;
				pw_stream_add_listener(capture, &captureListener, &captureEvents, this);

				if(connectStream(capture, config.inChannels, PW_DIRECTION_INPUT) < 0) {
					close();
					return Result{Error::DeviceOpenFailed, "Failed to connect PipeWire capture stream"};
				}
			}

			return Ok;
		}

		Result close() override {
			if(playback) {
				spa_hook_remove(&playbackListener);
				pw_stream_destroy(playback);
				playback = nullptr;
			}

			if(capture) {
				spa_hook_remove(&captureListener);
				pw_stream_destroy(capture);
				capture = nullptr;
			}

			if(core) {
				pw_core_disconnect(core);
				core = nullptr;
			}

			if(context) {
				pw_context_destroy(context);
				context = nullptr;
			}

			if(loop) {
				pw_thread_loop_destroy(loop);
				loop = nullptr;
			}

			outScratch.clear();
			inScratch.clear();
			outPtrs.clear();
			inPtrs.clear();
			pw_deinit();
			return Ok;
		}

	protected:
		void run() override {
			if(!loop) {
				running.store(false, std::memory_order_release);
				return;
			}

			if(pw_thread_loop_start(loop) < 0) {
				running.store(false, std::memory_order_release);
				return;
			}

			while(running.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}

			pw_thread_loop_stop(loop);
		}

	private:
		pw_stream* createStream(const char* category, const char* role) {
			return pw_stream_new(
				core,
				config.name.empty() ? "mka-pipewire" : config.name.c_str(),
				pw_properties_new(
					PW_KEY_MEDIA_TYPE, "Audio",
					PW_KEY_MEDIA_CATEGORY, category,
					PW_KEY_MEDIA_ROLE, role,
					nullptr
				)
			);
		}

		int connectStream(pw_stream* s, uint32_t channels, pw_direction direction) {
			spa_audio_info_raw audioInfo{};
			audioInfo.format = formatSpec.spa;
			audioInfo.rate = config.samplerate;
			audioInfo.channels = channels;
			for(uint32_t ch = 0; ch < channels && ch < SPA_AUDIO_MAX_CHANNELS; ++ch) {
				audioInfo.position[ch] = SPA_AUDIO_CHANNEL_UNKNOWN;
			}

			uint8_t podBuffer[1024]{};
			spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
			const spa_pod* params[1] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audioInfo) };

			const pw_stream_flags flags = static_cast<pw_stream_flags>(
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_MAP_BUFFERS |
				PW_STREAM_FLAG_RT_PROCESS
			);

			return pw_stream_connect(s, direction, PW_ID_ANY, flags, params, 1);
		}

		static void onPlaybackProcess(void* data) {
			auto* self = static_cast<PipeWire*>(data);
			if(self) self->process(true);
		}

		static void onCaptureProcess(void* data) {
			auto* self = static_cast<PipeWire*>(data);
			if(self) self->process(false);
		}


		static void* dataPtr(spa_data& data) {
			auto* bytes = static_cast<uint8_t*>(data.data);
			const uint32_t offset = (data.chunk ? data.chunk->offset : 0);
			return bytes ? bytes + offset : nullptr;
		}

		uint32_t availableFrames(const spa_buffer* buffer, bool captureSide) const {
			if(!buffer) return 0;
			uint32_t frames = config.bufferSize;

			const uint32_t channels = captureSide ? config.inChannels : config.outChannels;
			for(uint32_t ch = 0; ch < channels; ++ch) {
				const spa_data& data = buffer->datas[ch];
				uint32_t bytes = data.maxsize;
				if(captureSide && data.chunk && data.chunk->size > 0) {
					bytes = data.chunk->size;
				}
				frames = std::min(frames, bytes / formatSpec.bytesPerSample);
			}

			return frames;
		}

		void process(bool playbackTrigger) {
			(void)playbackTrigger;

			pw_buffer* outBuffer = nullptr;
			pw_buffer* inBuffer = nullptr;

			if(playback) outBuffer = pw_stream_dequeue_buffer(playback);
			if(capture) inBuffer = pw_stream_dequeue_buffer(capture);

			const bool readyOut = outBuffer != nullptr;
			const bool readyIn = inBuffer != nullptr;
			if(!readyOut && !readyIn) return;

			uint32_t frames = config.bufferSize;
			if(readyOut) frames = std::min(frames, availableFrames(outBuffer->buffer, false));
			if(readyIn)  frames = std::min(frames, availableFrames(inBuffer->buffer, true));

			if(frames == 0) {
				if(readyOut) pw_stream_queue_buffer(playback, outBuffer);
				if(readyIn) pw_stream_queue_buffer(capture, inBuffer);
				return;
			}

			if(readyIn) {
				for(uint32_t ch = 0; ch < config.inChannels; ++ch) {
					spa_data& data = inBuffer->buffer->datas[ch];
					decodeToFloat(dataPtr(data), inPtrs[ch], frames, formatSpec);
				}
			}

			Block block {
				.samplerate = config.samplerate,
				.outChannels = readyOut ? config.outChannels : 0,
				.inChannels = readyIn ? config.inChannels : 0,
				.frames = frames,
				.out = readyOut ? outPtrs.data() : nullptr,
				.in = readyIn ? inPtrs.data() : nullptr
			};

			if(callback) {
				callback(block);
			} else if(readyOut) {
				for(uint32_t ch = 0; ch < block.outChannels; ++ch) {
					std::memset(block.out[ch], 0, block.frames * sizeof(float));
				}
			}

			if(readyOut) {
				for(uint32_t ch = 0; ch < config.outChannels; ++ch) {
					spa_data& data = outBuffer->buffer->datas[ch];
					encodeFromFloat(outPtrs[ch], dataPtr(data), frames, formatSpec);
					if(data.chunk) {
						data.chunk->offset = 0;
						data.chunk->stride = formatSpec.bytesPerSample;
						data.chunk->size = frames * formatSpec.bytesPerSample;
					}
				}
				pw_stream_queue_buffer(playback, outBuffer);
			}

			if(readyIn) {
				pw_stream_queue_buffer(capture, inBuffer);
			}
		}

		AudioFormatSpec formatSpec { SampleEncoding::Float32, SPA_AUDIO_FORMAT_F32P, 4 };

		pw_thread_loop* loop = nullptr;
		pw_context* context = nullptr;
		pw_core* core = nullptr;

		pw_stream* playback = nullptr;
		pw_stream* capture = nullptr;
		spa_hook playbackListener {};
		spa_hook captureListener {};
		pw_stream_events playbackEvents {};
		pw_stream_events captureEvents {};

		std::vector<std::vector<float>> outScratch;
		std::vector<std::vector<float>> inScratch;
		std::vector<float*> outPtrs;
		std::vector<float*> inPtrs;
	};
}
