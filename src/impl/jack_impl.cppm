module;

#include <jack/jack.h>

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module audio.jack;
export import audio.block;
export import audio.config;
export import audio.error;
import audio.constants;
import audio.abstract_core;

namespace mka::audio {
	
	// JACK callbacks
	int sampleRateCallback(jack_nframes_t nframes, void* arg);
	int bufferSizeCallback(jack_nframes_t nframes, void* arg);
	int processCallback(jack_nframes_t nframes, void* arg);
	int xrunCallback(void* arg);
	void shutdownCallback(void* arg);

	struct JackChannelHandle {
		Channel			channel;
		jack_port_t*	port = nullptr;
	};
		
	export class JACK final : public AbstractCoreAudio {

	public:

		JACK() {
			// The channel bank is large (~34 MiB with current constants), so it must live on the heap.
			// Keeping it as a stack member causes a stack overflow during object construction.
			openedChannels = std::make_unique<JackChannelHandle[]>(constants::MAX_CHANNEL_COUNT);

			jack_status_t status {};
			client = jack_client_open("mka_audio_client", JackNoStartServer, &status);

			if(!client) {
				state.store(State::Closed);
				return;
			}

			// setting callbacks
			jack_on_shutdown(client, shutdownCallback, this);
			jack_set_xrun_callback(client, xrunCallback, this);
			jack_set_process_callback(client, processCallback, this);
			jack_set_sample_rate_callback(client, sampleRateCallback, this);
			jack_set_buffer_size_callback(client, bufferSizeCallback, this);

			// setting JACK API value
			const uint32_t _jackSampleRate = jack_get_sample_rate(client);
			const uint32_t _jackBufferSize = jack_get_buffer_size(client);
	
			// setting audio engine value
			jackSampleRate.store(_jackSampleRate);
			jackBufferSize.store(_jackBufferSize);
			sampleRate.store(_jackSampleRate);
			blockSize.store(_jackBufferSize);
			
			state.store(State::Stopped);
		}

		~JACK() override {
			close();
		}

		std::vector<ChannelInfo> getChannels() override {
			if (!client) return {};
		
			const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical);
			if (!ports) return {};

			std::vector<ChannelInfo> channels;
			
			size_t count = 0;
			while (ports[count]) ++count;

			channels.reserve(count);	
			
			for (int i = 0; ports[i]; ++i) {

				jack_port_t* port = jack_port_by_name(client, ports[i]);
				if (!port) continue;
				
				ChannelInfo info{};

				// Copy the name
				std::strncpy(info.name, ports[i], sizeof(info.name) - 1);
				info.name[sizeof(info.name) - 1] = '\0';

				const unsigned long flags = jack_port_flags(port);
				info.direction = (flags & JackPortIsOutput) 
									? Direction::In : Direction::Out;

				channels.emplace_back(info);
			}

			jack_free(ports);

			std::ranges::sort(channels, [](const ChannelInfo& a, const ChannelInfo& b) {
					return std::strcmp(a.name, b.name) < 0;
			});

			return channels;
		}

		Result open(const ChannelInfo channel) override {
			
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if(!client) {
				return Result { Error::DeviceOpenFailed, "JACK client unavailable." };
			}
			
			if(state.load() != State::Stopped) {
				return Result { Error::WouldBlock, "Engine must be stopped before opening a channel." };
			}
			
			size_t chanCount = channelCount.load();
			if (chanCount >= constants::MAX_CHANNEL_COUNT) {
				return Result { Error::GenericError, "Maximum channel count reached." };
			}

			// Check if channel has been already opened
			for (size_t i = 0; i < chanCount; ++i) {
		        if (std::strcmp(openedChannels[i].channel.channelInfo.name, channel.name) == 0) {
				    return Result{ Error::AlreadyExists, "Channel already opened." };
				}
			}

			std::string portName = (channel.direction == Direction::In ? "input_" : "output_");
			portName += std::to_string(channel.direction == Direction::In ? inputCounter++ : outputCounter++);

			jack_port_t* port = jack_port_register(
				client, 
				portName.c_str(), 
				JACK_DEFAULT_AUDIO_TYPE, 
				channel.direction == Direction::In ? JackPortIsInput : JackPortIsOutput,
				0
			);

			if(!port) {
				return Result { Error::DeviceOpenFailed, "Failed to register JACK port." };
			}

			JackChannelHandle &handle = openedChannels[chanCount];

			std::strncpy(handle.channel.channelInfo.name, channel.name, sizeof(handle.channel.channelInfo.name) - 1);
			handle.channel.channelInfo.name[sizeof(handle.channel.channelInfo.name) - 1] = '\0';
			handle.channel.channelInfo.direction = channel.direction;
			
			handle.channel.deviceInfo.sampleRate = jack_get_sample_rate(client);
			handle.channel.deviceInfo.bufferSize = jack_get_buffer_size(client);
			handle.channel.deviceInfo.format	 = Format::Float32;

			handle.port = port;
				
			const int err = channel.direction == Direction::In
				? jack_connect(client, channel.name, jack_port_name(port))
				: jack_connect(client, jack_port_name(port), channel.name);	
			
			if (err != 0) {
				jack_port_unregister(client, port);

				return Result { Error::DeviceOpenFailed, "Failed to connect JACK port." };
			}

			channelCount.store(chanCount + 1, std::memory_order_release);
			
			return mka::audio::Ok;
		}

		void start() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (!client || state.load() != State::Stopped) return;

			state.store(State::Starting);

			if (jack_activate(client) == 0) {
				state.store(State::Running);
				return;
			}

			state.store(State::Stopped);
		}

		void stop() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();
		}

		Result close() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();

			if (client) {
				const size_t count = channelCount.load(std::memory_order_acquire);
				for (size_t i = 0; i < count; ++i) {
					auto& ch = openedChannels[i];
					if (!ch.port) continue;
					jack_port_unregister(client, ch.port);
					ch.port = nullptr;
				}
				
				channelCount.store(0);

				jack_client_close(client);
				client = nullptr;
				state.store(State::Closed);
			}
			return mka::audio::Ok;
		}

	protected:
		void run() override {}

	private:
		
		void stopNoLock() {
			if (!client || state.load() != State::Running) return;
			state.store(State::Stopping);
			jack_deactivate(client);
			state.store(State::Stopped);
		}

	private:
		friend int bufferSizeCallback(jack_nframes_t nframes, void* arg);
		friend int sampleRateCallback(jack_nframes_t nframes, void* arg);
		friend int processCallback(jack_nframes_t nframes, void* arg);
		friend int xrunCallback(void* arg);
		friend void shutdownCallback(void* arg);

		jack_client_t* client = nullptr;
		std::unique_ptr<JackChannelHandle[]> openedChannels;
			
		size_t inputCounter = 0;
		size_t outputCounter = 0;
		
		std::atomic<size_t>	  channelCount	 = 0;
		std::atomic<uint32_t> jackSampleRate = 0;
		std::atomic<uint32_t> jackBufferSize = 0;
		std::atomic<uint64_t> xrunCount		 = 0;
		std::atomic<uint64_t> underrunCount	 = 0;
	};

	int sampleRateCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->jackSampleRate.store(nframes);
		return 0;
	}

	int bufferSizeCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->jackBufferSize.store(nframes);
		return 0;
	}

	void shutdownCallback(void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->state.store(State::Stopped);
	}

	int xrunCallback(void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->xrunCount.fetch_add(1);
		return 0;
	}

	int processCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
	
		if (!engine->callback) return 0;
	
		Block block {};
		block.blockSize = engine->blockSize.load();
		block.sampleRate = engine->sampleRate.load();
	
		const size_t channelCount = engine->channelCount.load(std::memory_order_acquire);
		size_t inputMap[constants::MAX_CHANNEL_COUNT] {};
		size_t outputMap[constants::MAX_CHANNEL_COUNT] {};
		size_t inputCount = 0;
		size_t outputCount = 0;
	
		// 1) Capture backend buffers and classify opened channels.
		for (size_t i = 0; i < channelCount; ++i) {
			auto& ch = engine->openedChannels[i];
	
			if (ch.channel.channelInfo.direction == Direction::In) {
				inputMap[inputCount++] = i;
	
				float* buffer = static_cast<float*>(jack_port_get_buffer(ch.port, nframes));
				if (!buffer) continue;
	
				const size_t copyCount = (nframes < constants::MAX_BLOCK_SIZE) ? nframes : constants::MAX_BLOCK_SIZE;
				std::memcpy(ch.channel.scratchBuffer, buffer, sizeof(float) * copyCount);
				ch.channel.fifo.push(ch.channel.scratchBuffer, copyCount);
			} else {
				outputMap[outputCount++] = i;
			}
		}
	
		block.inputCount = static_cast<uint32_t>(inputCount);
		block.outputCount = static_cast<uint32_t>(outputCount);
	
		// 2) Compute how many DSP blocks can be processed without spinning.
		//    IMPORTANT: with 0 input channels, we must process only a bounded number of blocks,
		//    otherwise the callback loops forever and JACK kills the client.
		size_t processIterations = 1;
		if (inputCount > 0) {
			processIterations = SIZE_MAX;
			for (size_t in = 0; in < inputCount; ++in) {
				auto& inChannel = engine->openedChannels[inputMap[in]].channel;
				const size_t availableBlocks = inChannel.fifo.available() / block.blockSize;
				if (availableBlocks < processIterations) processIterations = availableBlocks;
			}
		}
	
		for (size_t iter = 0; iter < processIterations; ++iter) {
			for (size_t in = 0; in < inputCount; ++in) {
				auto& inChannel = engine->openedChannels[inputMap[in]].channel;
				inChannel.fifo.pop(block.inputs[in], block.blockSize);
			}
	
			engine->callback(block);
	
			for (size_t out = 0; out < outputCount; ++out) {
				auto& outChannel = engine->openedChannels[outputMap[out]].channel;
				const size_t pushed = outChannel.fifo.push(block.outputs[out], block.blockSize);
				if (pushed < block.blockSize) {
					// FIFO full -> drop remainder to keep callback bounded and realtime-safe.
					engine->underrunCount.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
	
		// 3) Feed JACK output buffers from output FIFOs.
		for (size_t out = 0; out < outputCount; ++out) {
			auto& ch = engine->openedChannels[outputMap[out]];
	
			float* buffer = static_cast<float*>(jack_port_get_buffer(ch.port, nframes));
			if (!buffer) continue;
	
			const size_t toCopy = (ch.channel.fifo.available() < nframes) ? ch.channel.fifo.available() : nframes;
			ch.channel.fifo.pop(buffer, toCopy);
	
			// Zero-fill remaining frames on underrun to avoid garbage audio.
			if (toCopy < nframes) {
				for (size_t idx = toCopy; idx < nframes; ++idx) {
					buffer[idx] = 0.0f;
				}
			}
		}
	
		return 0;
	}

} // namespace mka::audio
	
