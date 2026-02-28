module;

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

export module audio.pipewire;
export import audio.block;
export import audio.config;
export import audio.error;
import audio.abstract_core;
import audio.constants;
import audio.realtime_pipeline;

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>


namespace mka::audio {

	struct DiscoveredChannel {
		ChannelInfo channelInfo {};
		uint32_t nodeId = SPA_ID_INVALID;
		uint32_t portId = SPA_ID_INVALID;
		uint32_t channelIndex = 0;
	};

	struct PipeWireChannelHandle {
		Channel channel {};
		uint32_t nodeId = SPA_ID_INVALID;
		uint32_t portId = SPA_ID_INVALID;
		uint32_t channelIndex = 0;
	};

	export class PipeWire final : public AbstractCoreAudio {
	public:
		PipeWire() {
			pw_init(nullptr, nullptr);

			openedChannels = std::make_unique<PipeWireChannelHandle[]>(constants::MAX_CHANNEL_COUNT);
			inputBlockStorage = std::make_unique<float[]>(constants::MAX_STATIC_BUFFER_SIZE);
			outputBlockStorage = std::make_unique<float[]>(constants::MAX_STATIC_BUFFER_SIZE);
			inputResampleScratch = std::make_unique<float[]>(constants::MAX_FIFO_SIZE);
			outputResampleScratch = std::make_unique<float[]>(constants::MAX_FIFO_SIZE);

			loop = pw_thread_loop_new("mka-pw-loop", nullptr);
			if (!loop) {
				state.store(State::Closed);
				return;
			}

			context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
			if (!context) {
				state.store(State::Closed);
				return;
			}

			core = pw_context_connect(context, nullptr, 0);
			if (!core) {
				state.store(State::Closed);
				return;
			}

			registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
			if (!registry) {
				state.store(State::Closed);
				return;
			}

			spa_zero(registryListener);
			spa_zero(coreListener);

			pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
			pw_core_add_listener(core, &coreListener, &coreEvents, this);

			if (pw_thread_loop_start(loop) < 0) {
				state.store(State::Closed);
				return;
			}

			// L'énumération PipeWire arrive via callbacks asynchrones.
			// On laisse quelques millisecondes au graphe pour publier les ports
			// avant le premier getChannels().
			std::this_thread::sleep_for(std::chrono::milliseconds(120));

			const uint32_t defaultRate = 48'000;
			const uint32_t defaultBuffer = 256;
			sampleRate.store(defaultRate);
			blockSize.store(defaultBuffer);
			backendSampleRate.store(defaultRate);
			backendBufferSize.store(defaultBuffer);

			state.store(State::Stopped);
		}

		~PipeWire() override {
			close();

			if (registry) {
				pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
				registry = nullptr;
			}

			if (core) {
				pw_core_disconnect(core);
				core = nullptr;
			}

			if (context) {
				pw_context_destroy(context);
				context = nullptr;
			}

			if (loop) {
				pw_thread_loop_stop(loop);
				pw_thread_loop_destroy(loop);
				loop = nullptr;
			}
		}

		std::vector<ChannelInfo> getChannels() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			std::vector<ChannelInfo> channels;
			channels.reserve(discoveredChannels.size());
			for (const auto& ch : discoveredChannels) {
				channels.push_back(ch.channelInfo);
			}

			std::ranges::sort(channels, [](const ChannelInfo& a, const ChannelInfo& b) {
				return std::strcmp(a.name, b.name) < 0;
			});

			return channels;
		}

		Result open(const ChannelInfo channel) override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (state.load() != State::Stopped) {
				return Result { Error::WouldBlock, "Engine must be stopped before opening a PipeWire channel." };
			}

			const size_t count = channelCount.load(std::memory_order_acquire);
			if (count >= constants::MAX_CHANNEL_COUNT) {
				return Result { Error::GenericError, "Maximum channel count reached." };
			}

			for (size_t i = 0; i < count; ++i) {
				if (std::strcmp(openedChannels[i].channel.channelInfo.name, channel.name) == 0) {
					return Result { Error::AlreadyExists, "Channel already opened." };
				}
			}

			auto it = std::find_if(discoveredChannels.begin(), discoveredChannels.end(), [&](const DiscoveredChannel& d) {
				return d.channelInfo.direction == channel.direction
					&& std::strcmp(d.channelInfo.name, channel.name) == 0;
			});

			if (it == discoveredChannels.end()) {
				return Result { Error::DeviceNotFound, "PipeWire channel not found." };
			}

			PipeWireChannelHandle& handle = openedChannels[count];
			std::memcpy(&handle.channel.channelInfo, &it->channelInfo, sizeof(ChannelInfo));
			handle.nodeId = it->nodeId;
			handle.portId = it->portId;
			handle.channelIndex = it->channelIndex;
			handle.channel.deviceInfo.sampleRate = backendSampleRate.load(std::memory_order_relaxed);
			handle.channel.deviceInfo.bufferSize = backendBufferSize.load(std::memory_order_relaxed);
			handle.channel.deviceInfo.format = Format::Float32;
			handle.channel.inputResampler.configure(handle.channel.deviceInfo.sampleRate, sampleRate.load());
			handle.channel.outputResampler.configure(sampleRate.load(), handle.channel.deviceInfo.sampleRate);

			channelCount.store(count + 1, std::memory_order_release);
			if (channel.direction == Direction::In) {
				inputCount.fetch_add(1, std::memory_order_relaxed);
			} else {
				outputCount.fetch_add(1, std::memory_order_relaxed);
			}

			rebuildOpenedNodeCacheNoLock();

			return Ok;
		}

		void start() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (state.load() != State::Stopped || !core || !loop) {
				return;
			}

			if (!createStreamsNoLock()) {
				teardownStreamsNoLock();
				state.store(State::Stopped);
				return;
			}

			state.store(State::Running);
		}

		void stop() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (state.load() != State::Running) return;
			state.store(State::Stopping);
			teardownStreamsNoLock();
			state.store(State::Stopped);
		}

		RuntimeStats getRuntimeStats() const override {
			RuntimeStats stats {};
			stats.xrunCount = xrunCount.load(std::memory_order_relaxed);
			stats.underrunCount = underrunCount.load(std::memory_order_relaxed);
			stats.outputMissingFrames = outputMissingFrames.load(std::memory_order_relaxed);
			stats.backendSampleRate = backendSampleRate.load(std::memory_order_relaxed);
			stats.backendBufferSize = backendBufferSize.load(std::memory_order_relaxed);
			stats.openedChannels = channelCount.load(std::memory_order_relaxed);
			stats.openedInputs = inputCount.load(std::memory_order_relaxed);
			stats.openedOutputs = outputCount.load(std::memory_order_relaxed);
			return stats;
		}

		Result close() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (state.load() == State::Running) {
				state.store(State::Stopping);
				teardownStreamsNoLock();
			}

			xrunCount.store(0);
			underrunCount.store(0);
			outputMissingFrames.store(0);
			channelCount.store(0);
			inputCount.store(0);
			outputCount.store(0);
			openedNodeCache.clear();

			if (state.load() != State::Closed) {
				state.store(State::Stopped);
			}
			return Ok;
		}

	protected:
		void run() override {}

	private:
		static void onCoreDone(void*, uint32_t, int) {}

		static void onRegistryGlobal(void* data,
			uint32_t id,
			uint32_t,
			const char* type,
			uint32_t,
			const spa_dict* props) {
			auto* engine = static_cast<PipeWire*>(data);
			if (!engine || !props || !type) return;

			if (std::strcmp(type, PW_TYPE_INTERFACE_Port) != 0) {
				return;
			}

			std::lock_guard<std::mutex> lock(engine->lifecycleMutex);

			const char* nodeIdText = spa_dict_lookup(props, PW_KEY_NODE_ID);
			const char* directionText = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
			const char* portName = spa_dict_lookup(props, PW_KEY_PORT_NAME);
			const char* channelText = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
			if (!nodeIdText || !directionText || !portName) {
				return;
			}

			char* end = nullptr;
			const unsigned long parsedNode = std::strtoul(nodeIdText, &end, 10);
			if (!end || *end != '\0') return;

			const Direction direction = (std::strcmp(directionText, "in") == 0) ? Direction::Out : Direction::In;
			uint32_t channelIndex = 0;
			if (channelText) {
				channelIndex = static_cast<uint32_t>(std::hash<std::string_view>{}(channelText) & 0xFFFFu);
			}

			DiscoveredChannel discovered {};
			discovered.nodeId = static_cast<uint32_t>(parsedNode);
			discovered.portId = id;
			discovered.channelIndex = channelIndex;
			std::snprintf(
				discovered.channelInfo.name,
				sizeof(discovered.channelInfo.name),
				"pw:%u:%u:%s",
				discovered.nodeId,
				discovered.portId,
				portName
			);
			discovered.channelInfo.direction = direction;

			auto existing = std::find_if(
				engine->discoveredChannels.begin(),
				engine->discoveredChannels.end(),
				[id](const DiscoveredChannel& current) { return current.portId == id; }
			);

			if (existing != engine->discoveredChannels.end()) {
				*existing = discovered;
				return;
			}

			engine->discoveredChannels.push_back(discovered);
		}

		static void onOutputProcess(void* data) {
			auto* engine = static_cast<PipeWire*>(data);
			if (!engine) return;
			engine->processOutputNoLock();
		}

		static void onInputProcess(void* data) {
			auto* engine = static_cast<PipeWire*>(data);
			if (!engine) return;
			engine->processInputNoLock();
		}

		void refreshRegistryNoLock() {
			(void)core;
			(void)loop;
			// La registry est alimentée de manière asynchrone par les callbacks PipeWire.
			// Cette fonction garde l'API symétrique avec JACK/ALSA pour de futures
			// politiques de refresh explicites (sync séquencée, timeout configurable).
		}

		bool createStreamsNoLock() {
			const uint32_t inCount = static_cast<uint32_t>(inputCount.load(std::memory_order_relaxed));
			const uint32_t outCount = static_cast<uint32_t>(outputCount.load(std::memory_order_relaxed));
			if (inCount == 0 && outCount == 0) {
				return false;
			}

			pw_thread_loop_lock(loop);

			if (inCount > 0) {
				captureStream = pw_stream_new(core, "mka-pw-capture", nullptr);
				if (!captureStream) {
					pw_thread_loop_unlock(loop);
					return false;
				}

				spa_zero(captureStreamListener);
				pw_stream_add_listener(captureStream, &captureStreamListener, &captureEvents, this);

				if (!connectStreamNoLock(captureStream, PW_DIRECTION_INPUT, inCount)) {
					pw_thread_loop_unlock(loop);
					return false;
				}
			}

			if (outCount > 0) {
				playbackStream = pw_stream_new(core, "mka-pw-playback", nullptr);
				if (!playbackStream) {
					pw_thread_loop_unlock(loop);
					return false;
				}

				spa_zero(playbackStreamListener);
				pw_stream_add_listener(playbackStream, &playbackStreamListener, &playbackEvents, this);

				if (!connectStreamNoLock(playbackStream, PW_DIRECTION_OUTPUT, outCount)) {
					pw_thread_loop_unlock(loop);
					return false;
				}
			}

			pw_thread_loop_unlock(loop);
			return true;
		}

		bool connectStreamNoLock(pw_stream* stream, pw_direction direction, uint32_t channels) {
			uint8_t buffer[1024] {};
			spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

			spa_audio_info_raw info {};
			info.format = SPA_AUDIO_FORMAT_F32;
			info.rate = sampleRate.load(std::memory_order_relaxed);
			info.channels = channels;
			for (uint32_t i = 0; i < channels && i < SPA_AUDIO_MAX_CHANNELS; ++i) {
				info.position[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
			}

			const spa_pod* params[1];
			params[0] = reinterpret_cast<const spa_pod*>(spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info));

			const pw_stream_flags flags = static_cast<pw_stream_flags>(
				PW_STREAM_FLAG_AUTOCONNECT
				| PW_STREAM_FLAG_MAP_BUFFERS
				| PW_STREAM_FLAG_RT_PROCESS
			);

			const int rc = pw_stream_connect(stream, direction, PW_ID_ANY, flags, params, 1);
			if (rc < 0) {
				return false;
			}

			backendSampleRate.store(info.rate, std::memory_order_relaxed);
			backendBufferSize.store(blockSize.load(std::memory_order_relaxed), std::memory_order_relaxed);
			return true;
		}

		void teardownStreamsNoLock() {
			if (!loop) return;
			pw_thread_loop_lock(loop);

			if (captureStream) {
				pw_stream_destroy(captureStream);
				captureStream = nullptr;
			}

			if (playbackStream) {
				pw_stream_destroy(playbackStream);
				playbackStream = nullptr;
			}

			pw_thread_loop_unlock(loop);
		}

		void processInputNoLock() {
			if (!captureStream || state.load(std::memory_order_relaxed) != State::Running) return;

			pw_buffer* buffer = pw_stream_dequeue_buffer(captureStream);
			if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0) return;

			spa_buffer* spaBuffer = buffer->buffer;
			spa_data* data = &spaBuffer->datas[0];
			if (!data->data) {
				pw_stream_queue_buffer(captureStream, buffer);
				return;
			}

			const size_t inCount = inputCount.load(std::memory_order_acquire);
			if (inCount == 0) {
				pw_stream_queue_buffer(captureStream, buffer);
				return;
			}

			const uint32_t strideBytes = (data->chunk && data->chunk->stride > 0)
				? data->chunk->stride
				: static_cast<uint32_t>(sizeof(float) * inCount);
			const size_t frameCount = (data->chunk && strideBytes > 0)
				? (data->chunk->size / strideBytes)
				: 0;

			const float* interleaved = static_cast<const float*>(data->data);

			for (size_t ch = 0; ch < inCount && ch < constants::MAX_CHANNEL_COUNT && ch < openedNodeCache.inputs.size(); ++ch) {
				float* deinterleaved = inputBlockStorage.get() + (ch * constants::MAX_BLOCK_SIZE);
				const size_t boundedFrames = std::min(frameCount, constants::MAX_BLOCK_SIZE);
				for (size_t i = 0; i < boundedFrames; ++i) {
					deinterleaved[i] = interleaved[(i * inCount) + ch];
				}

				realtime::ingestInput(
					openedNodeCache.inputs[ch]->channel,
					deinterleaved,
					boundedFrames,
					inputResampleScratch.get(),
					constants::MAX_FIFO_SIZE
				);
			}

			pw_stream_queue_buffer(captureStream, buffer);
		}

		void processOutputNoLock() {
			if (!playbackStream || state.load(std::memory_order_relaxed) != State::Running) return;

			pw_buffer* buffer = pw_stream_dequeue_buffer(playbackStream);
			if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0) return;

			spa_buffer* spaBuffer = buffer->buffer;
			spa_data* data = &spaBuffer->datas[0];
			if (!data->data) {
				pw_stream_queue_buffer(playbackStream, buffer);
				return;
			}

			const size_t outCount = outputCount.load(std::memory_order_acquire);
			if (outCount == 0) {
				pw_stream_queue_buffer(playbackStream, buffer);
				return;
			}

			const uint32_t strideBytes = (data->chunk && data->chunk->stride > 0)
				? data->chunk->stride
				: static_cast<uint32_t>(sizeof(float) * outCount);
			const size_t frameCount = (data->chunk && strideBytes > 0)
				? (data->chunk->size / strideBytes)
				: 0;

			const uint32_t fixedBlockSize = blockSize.load(std::memory_order_acquire);
			const size_t iterations = realtime::computeCallbackIterations(
				std::span<Channel*>(openedNodeCache.inputViews.data(), openedNodeCache.inputViews.size()),
				std::span<Channel*>(openedNodeCache.outputViews.data(), openedNodeCache.outputViews.size()),
				frameCount,
				fixedBlockSize
			);

			realtime::runEngine(
				callback,
				sampleRate.load(std::memory_order_acquire),
				fixedBlockSize,
				std::span<Channel*>(openedNodeCache.inputViews.data(), openedNodeCache.inputViews.size()),
				std::span<Channel*>(openedNodeCache.outputViews.data(), openedNodeCache.outputViews.size()),
				inputBlockStorage.get(),
				outputBlockStorage.get(),
				iterations
			);

			float* interleaved = static_cast<float*>(data->data);
			const size_t boundedFrames = std::min(frameCount, constants::MAX_BLOCK_SIZE);
			for (size_t ch = 0; ch < outCount && ch < constants::MAX_CHANNEL_COUNT && ch < openedNodeCache.outputs.size(); ++ch) {
				float* render = outputBlockStorage.get() + (ch * constants::MAX_BLOCK_SIZE);
				const size_t missing = realtime::renderOutput(
					openedNodeCache.outputs[ch]->channel,
					render,
					boundedFrames,
					outputResampleScratch.get(),
					constants::MAX_FIFO_SIZE
				);

				if (missing > 0) {
					underrunCount.fetch_add(1, std::memory_order_relaxed);
					outputMissingFrames.fetch_add(missing, std::memory_order_relaxed);
				}

				for (size_t i = 0; i < boundedFrames; ++i) {
					interleaved[(i * outCount) + ch] = render[i];
				}
			}

			if (data->chunk) {
				data->chunk->offset = 0;
				data->chunk->stride = sizeof(float) * outCount;
				data->chunk->size = boundedFrames * data->chunk->stride;
			}

			pw_stream_queue_buffer(playbackStream, buffer);
		}

		void rebuildOpenedNodeCacheNoLock() {
			openedNodeCache.clear();

			const size_t count = channelCount.load(std::memory_order_acquire);
			for (size_t i = 0; i < count; ++i) {
				auto* handle = &openedChannels[i];
				if (handle->channel.channelInfo.direction == Direction::In) {
					openedNodeCache.inputs.push_back(handle);
					openedNodeCache.inputViews.push_back(&handle->channel);
				} else {
					openedNodeCache.outputs.push_back(handle);
					openedNodeCache.outputViews.push_back(&handle->channel);
				}
			}
		}

	private:
		struct OpenedNodeCache {
			std::vector<PipeWireChannelHandle*> inputs;
			std::vector<PipeWireChannelHandle*> outputs;
			std::vector<Channel*> inputViews;
			std::vector<Channel*> outputViews;

			void clear() {
				inputs.clear();
				outputs.clear();
				inputViews.clear();
				outputViews.clear();
			}
		};


		pw_thread_loop* loop = nullptr;
		pw_context* context = nullptr;
		pw_core* core = nullptr;
		pw_registry* registry = nullptr;
		spa_hook registryListener {};
		spa_hook coreListener {};
		spa_hook captureStreamListener {};
		spa_hook playbackStreamListener {};
		pw_stream* captureStream = nullptr;
		pw_stream* playbackStream = nullptr;

		std::unique_ptr<PipeWireChannelHandle[]> openedChannels;
		std::vector<DiscoveredChannel> discoveredChannels;
		OpenedNodeCache openedNodeCache;
		std::unique_ptr<float[]> inputBlockStorage;
		std::unique_ptr<float[]> outputBlockStorage;
		std::unique_ptr<float[]> inputResampleScratch;
		std::unique_ptr<float[]> outputResampleScratch;

		std::atomic<size_t> channelCount = 0;
		std::atomic<size_t> inputCount = 0;
		std::atomic<size_t> outputCount = 0;
		std::atomic<uint32_t> backendSampleRate = 0;
		std::atomic<uint32_t> backendBufferSize = 0;
		std::atomic<size_t> xrunCount = 0;
		std::atomic<size_t> underrunCount = 0;
		std::atomic<size_t> outputMissingFrames = 0;

		inline static const pw_registry_events registryEvents {
			.version = PW_VERSION_REGISTRY_EVENTS,
			.global = onRegistryGlobal,
		};

		inline static const pw_core_events coreEvents {
			.version = PW_VERSION_CORE_EVENTS,
			.done = onCoreDone,
		};

		inline static const pw_stream_events captureEvents {
			.version = PW_VERSION_STREAM_EVENTS,
			.process = onInputProcess,
		};

		inline static const pw_stream_events playbackEvents {
			.version = PW_VERSION_STREAM_EVENTS,
			.process = onOutputProcess,
		};
	};

} // namespace mka::audio
