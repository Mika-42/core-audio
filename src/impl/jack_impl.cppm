module;

#include <jack/jack.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <span>

export module audio.jack;
export import audio.block;
export import audio.config;
export import audio.error;
import audio.constants;
import audio.realtime_pipeline;
import audio.abstract_core;
import audio.midi;

#include <jack/midiport.h>

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
	
	struct JackMidiMapping {
		char sourceName[256] {};
		uint8_t channel = 0;
		jack_port_t* port = nullptr;
	};

	export class JACK final : public AbstractCoreAudio {

	public:

		JACK() {

			openedChannels = std::make_unique<JackChannelHandle[]>(constants::MAX_CHANNEL_COUNT);
			inputBlockStorage = std::make_unique<float[]>(constants::MAX_STATIC_BUFFER_SIZE);
			outputBlockStorage = std::make_unique<float[]>(constants::MAX_STATIC_BUFFER_SIZE);
			inputResampleScratch = std::make_unique<float[]>(constants::MAX_FIFO_SIZE);
			outputResampleScratch = std::make_unique<float[]>(constants::MAX_FIFO_SIZE);

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

		std::vector<ChannelInfo> getMidiDevices() override {
			if (!client) return {};

			const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_MIDI_TYPE, JackPortIsPhysical);
			if (!ports) return {};

			std::vector<ChannelInfo> devices;
			for (size_t i = 0; ports[i]; ++i) {
				jack_port_t* port = jack_port_by_name(client, ports[i]);
				if (!port) continue;

				ChannelInfo info {};
				std::strncpy(info.name, ports[i], sizeof(info.name) - 1);
				info.name[sizeof(info.name) - 1] = '\0';

				const unsigned long flags = jack_port_flags(port);
				info.direction = (flags & JackPortIsOutput) ? Direction::In : Direction::Out;
				devices.emplace_back(info);
			}

			jack_free(ports);

			std::ranges::sort(devices, [](const ChannelInfo& a, const ChannelInfo& b) {
				return std::strcmp(a.name, b.name) < 0;
			});

			return devices;
		}

		Result mapMidiDevice(const ChannelInfo channel, uint8_t channelNum) override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (!client) {
				return Result { Error::DeviceOpenFailed, "JACK client unavailable." };
			}

			if (!channel.name || channel.name[0] == '\0') {
				return Result { Error::InvalidArgument, "MIDI device name must not be empty." };
			}

			if (channelNum >= constants::MAX_MIDI_DEVICE_MAPPINGS) {
				return Result { Error::OutOfRange, "MIDI channel must be in [0..15]." };
			}

			jack_port_t* sourcePort = jack_port_by_name(client, channel.name);
			if (!sourcePort) {
				return Result { Error::NotFound, "MIDI device not found." };
			}

			const unsigned long sourceFlags = jack_port_flags(sourcePort);
			if ((sourceFlags & JackPortIsOutput) == 0) {
				return Result { Error::InvalidArgument, "MIDI mapping expects an input device (JACK output port)." };
			}

			for (size_t i = 0; i < midiMappingCount; ++i) {
				if (std::strcmp(midiMappings[i].sourceName, channel.name) != 0) {
					continue;
				}

				// Reconfiguration path: keep realtime port alive and only change routing channel.
				midiMappings[i].channel = channelNum;
				return Ok;
			}

			if (midiMappingCount >= midiMappings.size()) {
				return Result { Error::GenericError, "Maximum MIDI mapping count reached." };
			}

			std::string portName = "mapped_midi_in_" + std::to_string(midiMappingCount);
			jack_port_t* mappedPort = jack_port_register(
				client,
				portName.c_str(),
				JACK_DEFAULT_MIDI_TYPE,
				JackPortIsInput,
				0
			);

			if (!mappedPort) {
				return Result { Error::DeviceOpenFailed, "Failed to register mapped MIDI input port." };
			}

			const int connectErr = jack_connect(client, channel.name, jack_port_name(mappedPort));
			if (connectErr != 0) {
				jack_port_unregister(client, mappedPort);
				return Result { Error::DeviceOpenFailed, "Failed to connect mapped MIDI input port." };
			}

			auto& mapping = midiMappings[midiMappingCount++];
			std::strncpy(mapping.sourceName, channel.name, sizeof(mapping.sourceName) - 1);
			mapping.sourceName[sizeof(mapping.sourceName) - 1] = '\0';
			mapping.channel = channelNum;
			mapping.port = mappedPort;
			return Ok;
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
			handle.channel.inputResampler.configure(handle.channel.deviceInfo.sampleRate, sampleRate.load());
			handle.channel.outputResampler.configure(sampleRate.load(), handle.channel.deviceInfo.sampleRate);

			handle.port = port;
				
			const int err = channel.direction == Direction::In
				? jack_connect(client, channel.name, jack_port_name(port))
				: jack_connect(client, jack_port_name(port), channel.name);	
			
			if (err != 0) {
				jack_port_unregister(client, port);

				return Result { Error::DeviceOpenFailed, "Failed to connect JACK port." };
			}

			channelCount.store(chanCount + 1, std::memory_order_release);			
			
			if(channel.direction == Direction::In) {
				inputCount++;
			}
			else if(channel.direction == Direction::Out) {
				outputCount++;
			}
			
			return mka::audio::Ok;
		}

		void start() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (!client || state.load() != State::Stopped) return;

			state.store(State::Starting);

			if (jack_activate(client) == 0) {
				midiTimeline.reset(0);
				state.store(State::Running);
				return;
			}

			state.store(State::Stopped);
		}

		void stop() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();
		}
		
		RuntimeStats getRuntimeStats() const override {
			RuntimeStats stats {};
			stats.xrunCount = xrunCount.load(std::memory_order_relaxed);
			stats.underrunCount = underrunCount.load(std::memory_order_relaxed);
			stats.outputMissingFrames = outputMissingFrames.load(std::memory_order_relaxed);
			stats.midiQueueOverflowCount = midiTimeline.queueOverflows();
			stats.midiBlockOverflowCount = midiTimeline.blockOverflows();
			stats.backendSampleRate = jackSampleRate.load(std::memory_order_relaxed);
			stats.backendBufferSize = jackBufferSize.load(std::memory_order_relaxed);
			stats.openedChannels = channelCount.load(std::memory_order_relaxed);
			stats.openedInputs = inputCount.load(std::memory_order_relaxed);
			stats.openedOutputs = outputCount.load(std::memory_order_relaxed);
			return stats;
		}

		Result close() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();
			
			xrunCount.store(0);
			underrunCount.store(0);
			outputMissingFrames.store(0);

			if (client) {
				const size_t count = channelCount.load(std::memory_order_acquire);
	
				for (size_t i = 0; i < count; ++i) {
					auto& ch = openedChannels[i];
					if (!ch.port) continue;
					jack_port_unregister(client, ch.port);
					ch.port = nullptr;
				}
				
				channelCount.store(0);
				inputCount.store(0);
				outputCount.store(0);
				
				for (size_t i = 0; i < midiMappingCount; ++i) {
					if (midiMappings[i].port) {
						jack_port_unregister(client, midiMappings[i].port);
						midiMappings[i].port = nullptr;
					}
					midiMappings[i].sourceName[0] = '\0';
					midiMappings[i].channel = 0;
				}
				midiMappingCount = 0;

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
		std::unique_ptr<float[]> inputBlockStorage;
		std::unique_ptr<float[]> outputBlockStorage;
		std::unique_ptr<float[]> inputResampleScratch;
		std::unique_ptr<float[]> outputResampleScratch;
		std::array<JackMidiMapping, constants::MAX_MIDI_DEVICE_MAPPINGS> midiMappings {};
		size_t midiMappingCount = 0;
		MidiTimeline midiTimeline {};

		size_t inputCounter = 0;
		size_t outputCounter = 0;
		
		std::atomic<size_t>		channelCount		= 0;
		std::atomic<size_t>		inputCount			= 0;
		std::atomic<size_t>		outputCount			= 0;
		std::atomic<uint32_t>	jackSampleRate		= 0;
		std::atomic<uint32_t>	jackBufferSize		= 0;
		std::atomic<size_t>		xrunCount			= 0;
		std::atomic<size_t>		underrunCount		= 0;
		std::atomic<size_t>		outputMissingFrames	= 0;
	};

	int sampleRateCallback(jack_nframes_t nframes, void* arg) {
		auto* engine = static_cast<JACK*>(arg);
		engine->jackSampleRate.store(nframes);

		const size_t channelCount = engine->channelCount.load(std::memory_order_acquire);
		const uint32_t engineRate = engine->sampleRate.load(std::memory_order_acquire);
		for (size_t i = 0; i < channelCount; ++i) {
			auto& channel = engine->openedChannels[i].channel;
			channel.deviceInfo.sampleRate = nframes;
			channel.inputResampler.configure(channel.deviceInfo.sampleRate, engineRate);
			channel.outputResampler.configure(engineRate, channel.deviceInfo.sampleRate);
		}

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
		if (nframes > constants::MAX_BLOCK_SIZE) return 0;

		const uint32_t fixedBlockSize = engine->blockSize.load(std::memory_order_acquire);
		if (fixedBlockSize == 0 || fixedBlockSize > constants::MAX_BLOCK_SIZE) {
			return 0;
		}

		size_t i = 0;
		
		const uint32_t engineSampleRate = engine->sampleRate.load(std::memory_order_acquire);

		const uint32_t backendSampleRate = engine->jackSampleRate.load(std::memory_order_acquire);

		const uint64_t callbackStartSample = engine->midiTimeline.currentSample();
			
		if (backendSampleRate > 0) {
			for (size_t mappingIndex = 0; mappingIndex < engine->midiMappingCount; ++mappingIndex) {
				const auto& mapping = engine->midiMappings[mappingIndex];
				if (!mapping.port) {
					continue;
				}

				void* midiBuffer = jack_port_get_buffer(mapping.port, nframes);
				if (!midiBuffer) {
					continue;
				}

				const uint32_t eventCount = jack_midi_get_event_count(midiBuffer);
				for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
					jack_midi_event_t jackEvent {};
					if (jack_midi_event_get(&jackEvent, midiBuffer, eventIndex) != 0) {
						continue;
					}

					if (!jackEvent.buffer || jackEvent.size == 0) {
						continue;
					}

					std::array<uint8_t, constants::MAX_MIDI_MESSAGE_SIZE> mappedBytes {};
					const size_t cappedSize = std::min(static_cast<size_t>(jackEvent.size), mappedBytes.size());
					for (size_t byteIndex = 0; byteIndex < cappedSize; ++byteIndex) {
						mappedBytes[byteIndex] = jackEvent.buffer[byteIndex];
					}

					// Apply channel routing only to MIDI channel voice messages (0x8n..0xEn).
					// System messages keep their status byte untouched.
					if (cappedSize > 0) {
						const uint8_t status = mappedBytes[0];
						if (status >= 0x80 && status <= 0xEF) {
							mappedBytes[0] = static_cast<uint8_t>((status & 0xF0u) | (mapping.channel & 0x0Fu));
						}
					}

					engine->midiTimeline.pushBackendOffsetEvent(
						callbackStartSample,
						jackEvent.time,
						engineSampleRate,
						backendSampleRate,
						mappedBytes.data(),
						cappedSize
					);
				}
			}
		}
		const size_t channelCount	= engine->channelCount.load(std::memory_order_acquire);
		const size_t inputCount		= engine->inputCount.load(std::memory_order_acquire);
		const size_t outputCount	= engine->outputCount.load(std::memory_order_acquire);
	
		if (inputCount > constants::MAX_CHANNEL_COUNT || outputCount > constants::MAX_CHANNEL_COUNT) {
			return 0;
		}

		
		JackChannelHandle* inputChannels[constants::MAX_CHANNEL_COUNT] {};
		JackChannelHandle* outputChannels[constants::MAX_CHANNEL_COUNT] {};
		Channel* inputChannelViews[constants::MAX_CHANNEL_COUNT] {};
		Channel* outputChannelViews[constants::MAX_CHANNEL_COUNT] {};
		size_t inIndex = 0;
		size_t outIndex = 0;
		
		for (i = 0; i < channelCount; ++i) {
	        auto& ch = engine->openedChannels[i];
			if (ch.channel.channelInfo.direction == Direction::In) {
				if (inIndex < inputCount) {
					inputChannels[inIndex] = &ch;
					inputChannelViews[inIndex++] = &ch.channel;
				}
				continue;
			}

			if (outIndex < outputCount) {
				outputChannels[outIndex] = &ch;
				outputChannelViews[outIndex++] = &ch.channel;
			}
		}

		if (inIndex != inputCount || outIndex != outputCount) {
			return 0;
		}

		for (i = 0; i < inputCount; ++i) {
			auto* ch = inputChannels[i];
			if(!ch || !ch->port) continue;

			float* backendBuffer = static_cast<float*>(jack_port_get_buffer(ch->port, nframes));
			if(!backendBuffer) continue;

			realtime::ingestInput(
				ch->channel,
				backendBuffer,
				nframes,
				engine->inputResampleScratch.get(),
				constants::MAX_FIFO_SIZE
			);
		}

		const size_t processIt = realtime::computeCallbackIterations(
			std::span<Channel*>(inputChannelViews, inputCount),
			std::span<Channel*>(outputChannelViews, outputCount),
			nframes,
			fixedBlockSize
		);

		realtime::runEngine(
			engine->callback,
			engineSampleRate,
			fixedBlockSize,
			std::span<Channel*>(inputChannelViews, inputCount),
			std::span<Channel*>(outputChannelViews, outputCount),
			engine->inputBlockStorage.get(),
			engine->outputBlockStorage.get(),
			processIt,
			&engine->midiTimeline
		);

		for (i = 0; i < outputCount; ++i) {
	        auto* ch = outputChannels[i];
	        if (!ch || !ch->port) continue;

		    float* buffer = static_cast<float*>(jack_port_get_buffer(ch->port, nframes));
			if (!buffer) continue;

			const size_t missingFrames = realtime::renderOutput(
				ch->channel,
				buffer,
				nframes,
				engine->outputResampleScratch.get(),
				constants::MAX_FIFO_SIZE
			);

			if (missingFrames > 0) {
				// Keep telemetry lock-free for realtime safety: one atomic increment for
				// event count and one for total missing samples.
				engine->underrunCount.fetch_add(1, std::memory_order_relaxed);
				engine->outputMissingFrames.fetch_add(missingFrames, std::memory_order_relaxed);
			}
		}

		return 0;
	}
} // namespace mka::audio
	
