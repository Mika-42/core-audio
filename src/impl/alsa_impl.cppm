module;

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <poll.h>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module audio.alsa;
export import audio.block;
export import audio.config;
export import audio.error;
import audio.abstract_core;
import audio.constants;
import audio.realtime_pipeline;

namespace mka::audio {

	namespace {
		inline void logAlsaError(const char* what, int code) {
			std::fprintf(stderr, "[ALSA] %s failed: %s (%d)\n", what, snd_strerror(code), code);
		}

		struct ParsedChannelName {
			std::string device;
			snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
			uint32_t channelIndex = 0;
		};

		struct EnumeratedDevice {
			std::string deviceId;
			std::string cardLabel;
			std::string playbackLabel;
			std::string captureLabel;
			uint32_t outputChannels = 0;
			uint32_t inputChannels = 0;
		};

		struct EnumeratedChannel {
			ChannelInfo channelInfo {};
			std::string deviceId;
			snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
			uint32_t channelIndex = 0;
		};

		bool parseChannelName(std::string_view name, ParsedChannelName& out) {
			// Encoding format is: <device>#<in|out>#<channel-index>
			// We use '#' as separator because ALSA device names naturally contain ':'
			// (example: hw:0,0), so ':' cannot be used safely for parsing.
			const size_t rightSep = name.rfind('#');
			if (rightSep == std::string_view::npos) return false;
			const size_t leftSep = name.rfind('#', rightSep - 1);
			if (leftSep == std::string_view::npos) return false;

			const std::string_view deviceView = name.substr(0, leftSep);
			const std::string_view streamView = name.substr(leftSep + 1, rightSep - leftSep - 1);
			const std::string_view indexView = name.substr(rightSep + 1);

			if (deviceView.empty() || streamView.empty() || indexView.empty()) return false;

			char* end = nullptr;
			const unsigned long parsed = std::strtoul(std::string(indexView).c_str(), &end, 10);
			if (!end || *end != '\0') return false;

			out.device = std::string(deviceView);
			out.channelIndex = static_cast<uint32_t>(parsed);

			if (streamView == "out") {
				out.stream = SND_PCM_STREAM_PLAYBACK;
				return true;
			}
			if (streamView == "in") {
				out.stream = SND_PCM_STREAM_CAPTURE;
				return true;
			}

			return false;
		}

		uint32_t queryMaxChannels(const char* device, snd_pcm_stream_t stream) {
			snd_pcm_t* handle = nullptr;
			const int openErr = snd_pcm_open(&handle, device, stream, SND_PCM_NONBLOCK);
			if (openErr < 0 || !handle) return 0;

			snd_pcm_hw_params_t* hw = nullptr;
			snd_pcm_hw_params_alloca(&hw);
			if (snd_pcm_hw_params_any(handle, hw) < 0) {
				snd_pcm_close(handle);
				return 0;
			}

			unsigned int maxChannels = 0;
			if (snd_pcm_hw_params_get_channels_max(hw, &maxChannels) < 0) {
				snd_pcm_close(handle);
				return 0;
			}

			snd_pcm_close(handle);
			// Some ALSA plugins report absurdly high theoretical channel counts.
			// Clamp to a DAW-friendly practical ceiling to avoid fake endpoints.
			if (maxChannels == 0) return 0;
			return std::min<uint32_t>(maxChannels, 32);
		}

		std::vector<EnumeratedDevice> enumeratePcmDevices() {
			std::vector<EnumeratedDevice> devices;

			int card = -1;
			if (snd_card_next(&card) < 0) {
				return devices;
			}

			while (card >= 0) {
				char cardName[32] {};
				std::snprintf(cardName, sizeof(cardName), "hw:%d", card);

				snd_ctl_t* ctl = nullptr;
				if (snd_ctl_open(&ctl, cardName, 0) >= 0 && ctl) {
					snd_ctl_card_info_t* cardInfo = nullptr;
					snd_ctl_card_info_alloca(&cardInfo);
					std::string cardLabel = cardName;
					if (snd_ctl_card_info(ctl, cardInfo) >= 0) {
						const char* cardReadable = snd_ctl_card_info_get_name(cardInfo);
						if (cardReadable && cardReadable[0] != '\0') {
							cardLabel = cardReadable;
						}
					}

					int dev = -1;
					while (true) {
						if (snd_ctl_pcm_next_device(ctl, &dev) < 0 || dev < 0) break;

						char devId[32] {};
						std::snprintf(devId, sizeof(devId), "hw:%d,%d", card, dev);

						const uint32_t outCh = queryMaxChannels(devId, SND_PCM_STREAM_PLAYBACK);
						const uint32_t inCh = queryMaxChannels(devId, SND_PCM_STREAM_CAPTURE);
						if (outCh == 0 && inCh == 0) {
							continue;
						}

						std::string playbackLabel = "Playback";
						std::string captureLabel = "Capture";
						snd_pcm_info_t* pcmInfo = nullptr;
						snd_pcm_info_alloca(&pcmInfo);

						snd_pcm_info_set_device(pcmInfo, dev);
						snd_pcm_info_set_subdevice(pcmInfo, 0);

						snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_PLAYBACK);
						if (snd_ctl_pcm_info(ctl, pcmInfo) >= 0) {
							const char* n = snd_pcm_info_get_name(pcmInfo);
							if (n && n[0] != '\0') playbackLabel = n;
						}

						snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_CAPTURE);
						if (snd_ctl_pcm_info(ctl, pcmInfo) >= 0) {
							const char* n = snd_pcm_info_get_name(pcmInfo);
							if (n && n[0] != '\0') captureLabel = n;
						}

						devices.push_back(EnumeratedDevice {
							.deviceId = devId,
							.cardLabel = cardLabel,
							.playbackLabel = playbackLabel,
							.captureLabel = captureLabel,
							.outputChannels = outCh,
							.inputChannels = inCh
						});
					}
					snd_ctl_close(ctl);
				}

				snd_card_next(&card);
			}

			if (devices.empty()) {
				const uint32_t fallbackOut = queryMaxChannels("default", SND_PCM_STREAM_PLAYBACK);
				const uint32_t fallbackIn = queryMaxChannels("default", SND_PCM_STREAM_CAPTURE);
				if (fallbackOut > 0 || fallbackIn > 0) {
					devices.push_back(EnumeratedDevice {
						.deviceId = "default",
						.cardLabel = "Default ALSA",
						.playbackLabel = "Default Playback",
						.captureLabel = "Default Capture",
						.outputChannels = fallbackOut,
						.inputChannels = fallbackIn
					});
				}
			}

			return devices;
		}

		float* channelPtr(const snd_pcm_channel_area_t* area, snd_pcm_uframes_t offset) {
			if (!area || !area->addr) return nullptr;

			return reinterpret_cast<float*>(
				static_cast<char*>(area->addr)
				+ (area->first / 8)
				+ (offset * (area->step / 8))
			);
		}
	}

	struct AlsaChannelHandle {
		Channel channel;
		uint32_t channelIndex = 0;
		snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
		std::string device;
	};

	export class ALSA final : public AbstractCoreAudio {

	public:
		ALSA() {
			openedChannels = std::make_unique<AlsaChannelHandle[]>(constants::MAX_CHANNEL_COUNT);
			inputBlockStorage = std::make_unique<float[]>(constants::MAX_STATIC_BUFFER_SIZE);
			outputBlockStorage = std::make_unique<float[]>(constants::MAX_STATIC_BUFFER_SIZE);
			inputResampleScratch = std::make_unique<float[]>(constants::MAX_FIFO_SIZE);
			outputResampleScratch = std::make_unique<float[]>(constants::MAX_FIFO_SIZE);

			sampleRate.store(48'000);
			blockSize.store(256);
			state.store(State::Stopped);
		}

		~ALSA() override {
			close();
		}

		std::vector<ChannelInfo> getChannels() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			std::vector<ChannelInfo> channels;
			discoveredChannels.clear();

			const std::vector<EnumeratedDevice> devices = enumeratePcmDevices();
			size_t total = 0;
			for (const auto& dev : devices) {
				total += dev.outputChannels + dev.inputChannels;
			}
			channels.reserve(total);
			discoveredChannels.reserve(total);

			for (const auto& dev : devices) {
				for (uint32_t i = 0; i < dev.outputChannels; ++i) {
					EnumeratedChannel entry {};
					std::snprintf(
						entry.channelInfo.name,
						sizeof(entry.channelInfo.name),
						"%s | %s | Out %u [%s]",
						dev.cardLabel.c_str(),
						dev.playbackLabel.c_str(),
						i + 1,
						dev.deviceId.c_str()
					);
					entry.channelInfo.direction = Direction::Out;
					entry.deviceId = dev.deviceId;
					entry.stream = SND_PCM_STREAM_PLAYBACK;
					entry.channelIndex = i;

					discoveredChannels.push_back(entry);
					channels.push_back(entry.channelInfo);
				}

				for (uint32_t i = 0; i < dev.inputChannels; ++i) {
					EnumeratedChannel entry {};
					std::snprintf(
						entry.channelInfo.name,
						sizeof(entry.channelInfo.name),
						"%s | %s | In %u [%s]",
						dev.cardLabel.c_str(),
						dev.captureLabel.c_str(),
						i + 1,
						dev.deviceId.c_str()
					);
					entry.channelInfo.direction = Direction::In;
					entry.deviceId = dev.deviceId;
					entry.stream = SND_PCM_STREAM_CAPTURE;
					entry.channelIndex = i;

					discoveredChannels.push_back(entry);
					channels.push_back(entry.channelInfo);
				}
			}

			return channels;
		}

		Result open(const ChannelInfo channel) override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);

			if (state.load() != State::Stopped) {
				return Result { Error::WouldBlock, "Engine must be stopped before opening ALSA channels." };
			}

			size_t count = channelCount.load(std::memory_order_acquire);
			if (count >= constants::MAX_CHANNEL_COUNT) {
				return Result { Error::GenericError, "Maximum channel count reached." };
			}

			for (size_t i = 0; i < count; ++i) {
				if (std::strcmp(openedChannels[i].channel.channelInfo.name, channel.name) == 0) {
					return Result { Error::AlreadyExists, "Channel already opened." };
				}
			}

			ParsedChannelName parsed {};
			bool parsedFromCache = false;
			for (const auto& entry : discoveredChannels) {
				if (std::strcmp(entry.channelInfo.name, channel.name) == 0
					&& entry.channelInfo.direction == channel.direction) {
					parsed.device = entry.deviceId;
					parsed.stream = entry.stream;
					parsed.channelIndex = entry.channelIndex;
					parsedFromCache = true;
					break;
				}
			}

			if (!parsedFromCache && !parseChannelName(channel.name, parsed)) {
				return Result { Error::GenericError, "Invalid ALSA channel name." };
			}

			const bool isInput = (channel.direction == Direction::In);
			if ((isInput && parsed.stream != SND_PCM_STREAM_CAPTURE)
				|| (!isInput && parsed.stream != SND_PCM_STREAM_PLAYBACK)) {
				return Result { Error::GenericError, "Direction mismatch in ALSA channel description." };
			}

			Result setupRet = isInput
				? ensureCaptureConfigured(parsed.device)
				: ensurePlaybackConfigured(parsed.device);
			if (!setupRet.ok()) {
				return setupRet;
			}

			const uint32_t maxSupported = isInput ? captureHardwareChannels : playbackHardwareChannels;
			if (parsed.channelIndex >= maxSupported) {
				return Result { Error::SetupHardwareParameterFailed, "Requested ALSA channel index exceeds hardware capacity." };
			}

			AlsaChannelHandle& handle = openedChannels[count];
			handle.channelIndex = parsed.channelIndex;
			handle.stream = parsed.stream;
			handle.device = parsed.device;
			handle.channel.deviceInfo.sampleRate = backendSampleRate.load(std::memory_order_acquire);
			handle.channel.deviceInfo.bufferSize = backendBufferSize.load(std::memory_order_acquire);
			handle.channel.deviceInfo.format = Format::Float32;
			handle.channel.inputResampler.configure(
				handle.channel.deviceInfo.sampleRate,
				sampleRate.load(std::memory_order_acquire)
			);
			handle.channel.outputResampler.configure(
				sampleRate.load(std::memory_order_acquire),
				handle.channel.deviceInfo.sampleRate
			);

			std::strncpy(handle.channel.channelInfo.name, channel.name, sizeof(handle.channel.channelInfo.name) - 1);
			handle.channel.channelInfo.name[sizeof(handle.channel.channelInfo.name) - 1] = '\0';
			handle.channel.channelInfo.direction = channel.direction;

			channelCount.store(count + 1, std::memory_order_release);
			if (isInput) {
				inputCount.fetch_add(1, std::memory_order_relaxed);
			} else {
				outputCount.fetch_add(1, std::memory_order_relaxed);
			}

			return Ok;
		}

		void start() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			if (state.load() != State::Stopped) return;
			if (!playback && !capture) return;

			state.store(State::Starting);
			workerStop.store(false, std::memory_order_release);
			worker = std::thread([this]() { run(); });
			state.store(State::Running);
		}

		void stop() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();
		}

		Result close() override {
			std::lock_guard<std::mutex> lock(lifecycleMutex);
			stopNoLock();

			xrunCount.store(0);
			underrunCount.store(0);
			outputMissingFrames.store(0);

			if (playback) {
				snd_pcm_close(playback);
				playback = nullptr;
			}
			if (capture) {
				snd_pcm_close(capture);
				capture = nullptr;
			}

			channelCount.store(0);
			inputCount.store(0);
			outputCount.store(0);
			pollDescriptors.clear();
			playbackDescriptorCount = 0;
			captureDescriptorCount = 0;
			playbackHardwareChannels = 0;
			captureHardwareChannels = 0;
			state.store(State::Closed);
			return Ok;
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

	protected:
		void run() override {
			if (playback) {
				const int prepErr = snd_pcm_prepare(playback);
				if (prepErr < 0) logAlsaError("snd_pcm_prepare(playback)", prepErr);
			}
			if (capture) {
				const int prepErr = snd_pcm_prepare(capture);
				if (prepErr < 0) logAlsaError("snd_pcm_prepare(capture)", prepErr);
				const int startErr = snd_pcm_start(capture);
				if (startErr < 0) logAlsaError("snd_pcm_start(capture)", startErr);
			}

			while (!workerStop.load(std::memory_order_acquire)) {
				if (pollDescriptors.empty()) {
					std::this_thread::yield();
					continue;
				}

				if (poll(pollDescriptors.data(), static_cast<nfds_t>(pollDescriptors.size()), 100) <= 0) {
					continue;
				}

				unsigned short reventsOut = 0;
				unsigned short reventsIn = 0;
				if (playback && playbackDescriptorCount > 0) {
					snd_pcm_poll_descriptors_revents(playback, pollDescriptors.data(), playbackDescriptorCount, &reventsOut);
				}
				if (capture && captureDescriptorCount > 0) {
					snd_pcm_poll_descriptors_revents(capture, pollDescriptors.data() + playbackDescriptorCount, captureDescriptorCount, &reventsIn);
				}

				if ((reventsOut | reventsIn) & POLLERR) {
					handleRecoverableXrun(playback);
					handleRecoverableXrun(capture);
					continue;
				}

				processCycle();
			}
		}

	private:
		void stopNoLock() {
			if (state.load() != State::Running) return;
			state.store(State::Stopping);
			workerStop.store(true, std::memory_order_release);
			if (worker.joinable()) {
				worker.join();
			}
			if (playback) snd_pcm_drop(playback);
			if (capture) snd_pcm_drop(capture);
			state.store(State::Stopped);
		}

		Result ensurePlaybackConfigured(const std::string& device) {
			if (playback) {
				if (playbackDevice == device) return Ok;
				return Result { Error::WouldBlock, "Playback device already selected. Close engine to switch ALSA device." };
			}
			return configurePcm(&playback, device, SND_PCM_STREAM_PLAYBACK, playbackDescriptorCount, playbackHardwareChannels);
		}

		Result ensureCaptureConfigured(const std::string& device) {
			if (capture) {
				if (captureDevice == device) return Ok;
				return Result { Error::WouldBlock, "Capture device already selected. Close engine to switch ALSA device." };
			}
			return configurePcm(&capture, device, SND_PCM_STREAM_CAPTURE, captureDescriptorCount, captureHardwareChannels);
		}

		Result configurePcm(
			snd_pcm_t** outHandle,
			const std::string& device,
			snd_pcm_stream_t stream,
			int& descriptorCount,
			uint32_t& hardwareChannels) {

			int err = snd_pcm_open(outHandle, device.c_str(), stream, SND_PCM_NONBLOCK);
			if (err < 0 || !(*outHandle)) {
				if (err < 0) logAlsaError("snd_pcm_open", err);
				return Result { Error::DeviceOpenFailed, snd_strerror(err) };
			}

			snd_pcm_hw_params_t* hw = nullptr;
			snd_pcm_hw_params_alloca(&hw);
			err = snd_pcm_hw_params_any(*outHandle, hw);
			if (err < 0) return failAndClose(outHandle, err, Error::HardwareSetupFailed, "snd_pcm_hw_params_any");

			err = snd_pcm_hw_params_set_access(*outHandle, hw, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_hw_params_set_access");

			err = snd_pcm_hw_params_set_format(*outHandle, hw, SND_PCM_FORMAT_FLOAT_LE);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_hw_params_set_format");

			unsigned int sampleRateRequest = sampleRate.load(std::memory_order_acquire);
			if (sampleRateRequest == 0) sampleRateRequest = 48'000;
			err = snd_pcm_hw_params_set_rate_near(*outHandle, hw, &sampleRateRequest, nullptr);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_hw_params_set_rate_near");

			snd_pcm_uframes_t periodRequest = blockSize.load(std::memory_order_acquire);
			if (periodRequest == 0) periodRequest = 256;
			periodRequest = std::min<snd_pcm_uframes_t>(periodRequest, constants::MAX_BLOCK_SIZE);
			err = snd_pcm_hw_params_set_period_size_near(*outHandle, hw, &periodRequest, nullptr);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_hw_params_set_period_size_near");

			snd_pcm_uframes_t bufferRequest = periodRequest * 2;
			err = snd_pcm_hw_params_set_buffer_size_near(*outHandle, hw, &bufferRequest);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_hw_params_set_buffer_size_near");

			const size_t alreadyOpened = channelCount.load(std::memory_order_acquire);
			unsigned int requestedChannels = static_cast<unsigned int>(std::max<size_t>(alreadyOpened + 1, 2));
			err = snd_pcm_hw_params_set_channels_near(*outHandle, hw, &requestedChannels);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_hw_params_set_channels_near");

			err = snd_pcm_hw_params(*outHandle, hw);
			if (err < 0) return failAndClose(outHandle, err, Error::HardwareSetupFailed, "snd_pcm_hw_params");

			unsigned int actualChannels = 0;
			snd_pcm_hw_params_get_channels(hw, &actualChannels);
			hardwareChannels = std::min<uint32_t>(actualChannels, constants::MAX_CHANNEL_COUNT);

			snd_pcm_sw_params_t* sw = nullptr;
			snd_pcm_sw_params_alloca(&sw);
			err = snd_pcm_sw_params_current(*outHandle, sw);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_sw_params_current");

			// Avail minimum equals one period to avoid wakeups smaller than our
			// realtime processing quantum.
			err = snd_pcm_sw_params_set_avail_min(*outHandle, sw, periodRequest);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_sw_params_set_avail_min");

			// Playback starts only when one period is queued, reducing startup xruns.
			if (stream == SND_PCM_STREAM_PLAYBACK) {
				err = snd_pcm_sw_params_set_start_threshold(*outHandle, sw, periodRequest);
			} else {
				err = snd_pcm_sw_params_set_start_threshold(*outHandle, sw, 1);
			}
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_sw_params_set_start_threshold");

			err = snd_pcm_sw_params(*outHandle, sw);
			if (err < 0) return failAndClose(outHandle, err, Error::SetupHardwareParameterFailed, "snd_pcm_sw_params");

			descriptorCount = snd_pcm_poll_descriptors_count(*outHandle);
			if (descriptorCount < 0) {
				return failAndClose(outHandle, descriptorCount, Error::PollSetupFailed, "snd_pcm_poll_descriptors_count");
			}

			refreshPollDescriptors();

			backendSampleRate.store(sampleRateRequest, std::memory_order_release);
			backendBufferSize.store(static_cast<uint32_t>(periodRequest), std::memory_order_release);
			if (sampleRate.load(std::memory_order_acquire) == 0) {
				sampleRate.store(sampleRateRequest, std::memory_order_release);
			}
			if (blockSize.load(std::memory_order_acquire) == 0) {
				blockSize.store(static_cast<uint32_t>(periodRequest), std::memory_order_release);
			}

			if (stream == SND_PCM_STREAM_PLAYBACK) {
				playbackDevice = device;
			} else {
				captureDevice = device;
			}

			return Ok;
		}

		void processCycle() {
			const uint32_t backendFramesMax = backendBufferSize.load(std::memory_order_acquire);
			if (backendFramesMax == 0 || backendFramesMax > constants::MAX_BLOCK_SIZE) return;

			const snd_pcm_channel_area_t* outAreas = nullptr;
			const snd_pcm_channel_area_t* inAreas = nullptr;
			snd_pcm_uframes_t outOffset = 0;
			snd_pcm_uframes_t inOffset = 0;
			snd_pcm_uframes_t outFrames = 0;
			snd_pcm_uframes_t inFrames = 0;

			bool readyOut = false;
			bool readyIn = false;

			if (playback) {
				snd_pcm_sframes_t avail = snd_pcm_avail_update(playback);
				if (avail < 0) {
					handleRecoverableXrun(playback, static_cast<int>(avail));
				} else if (avail > 0) {
					outFrames = std::min<snd_pcm_uframes_t>(backendFramesMax, static_cast<snd_pcm_uframes_t>(avail));
					int beginErr = snd_pcm_mmap_begin(playback, &outAreas, &outOffset, &outFrames);
					if (beginErr < 0) {
						handleRecoverableXrun(playback, beginErr);
					} else {
						readyOut = (outAreas != nullptr && outFrames > 0);
					}
				}
			}

			if (capture) {
				snd_pcm_sframes_t avail = snd_pcm_avail_update(capture);
				if (avail < 0) {
					handleRecoverableXrun(capture, static_cast<int>(avail));
				} else if (avail > 0) {
					inFrames = std::min<snd_pcm_uframes_t>(backendFramesMax, static_cast<snd_pcm_uframes_t>(avail));
					int beginErr = snd_pcm_mmap_begin(capture, &inAreas, &inOffset, &inFrames);
					if (beginErr < 0) {
						handleRecoverableXrun(capture, beginErr);
					} else {
						readyIn = (inAreas != nullptr && inFrames > 0);
					}
				}
			}

			if (!readyOut && !readyIn) return;

			snd_pcm_uframes_t backendFrames = backendFramesMax;
			if (readyOut) backendFrames = std::min(backendFrames, outFrames);
			if (readyIn) backendFrames = std::min(backendFrames, inFrames);
			if (backendFrames == 0) return;

			Channel* inputChannelViews[constants::MAX_CHANNEL_COUNT] {};
			Channel* outputChannelViews[constants::MAX_CHANNEL_COUNT] {};
			AlsaChannelHandle* inputHandles[constants::MAX_CHANNEL_COUNT] {};
			AlsaChannelHandle* outputHandles[constants::MAX_CHANNEL_COUNT] {};

			size_t inIndex = 0;
			size_t outIndex = 0;

			const size_t count = channelCount.load(std::memory_order_acquire);
			for (size_t i = 0; i < count; ++i) {
				AlsaChannelHandle& ch = openedChannels[i];
				if (ch.channel.channelInfo.direction == Direction::In) {
					inputHandles[inIndex] = &ch;
					inputChannelViews[inIndex++] = &ch.channel;
				} else {
					outputHandles[outIndex] = &ch;
					outputChannelViews[outIndex++] = &ch.channel;
				}
			}

			for (size_t i = 0; i < inIndex; ++i) {
				AlsaChannelHandle* ch = inputHandles[i];
				if (!ch || !readyIn) continue;
				if (ch->channelIndex >= captureHardwareChannels) continue;
				float* ptr = channelPtr(&inAreas[ch->channelIndex], inOffset);
				if (!ptr) continue;

				realtime::ingestInput(
					ch->channel,
					ptr,
					backendFrames,
					inputResampleScratch.get(),
					constants::MAX_FIFO_SIZE
				);
			}

			const uint32_t fixedBlockSize = blockSize.load(std::memory_order_acquire);
			const size_t iterations = realtime::computeCallbackIterations(
				std::span<Channel*>(inputChannelViews, inIndex),
				std::span<Channel*>(outputChannelViews, outIndex),
				backendFrames,
				fixedBlockSize
			);

			if (callback && fixedBlockSize > 0 && fixedBlockSize <= constants::MAX_BLOCK_SIZE) {
				realtime::runEngine(
					callback,
					sampleRate.load(std::memory_order_acquire),
					fixedBlockSize,
					std::span<Channel*>(inputChannelViews, inIndex),
					std::span<Channel*>(outputChannelViews, outIndex),
					inputBlockStorage.get(),
					outputBlockStorage.get(),
					iterations
				);
			}

			for (size_t i = 0; i < outIndex; ++i) {
				AlsaChannelHandle* ch = outputHandles[i];
				if (!ch || !readyOut) continue;
				if (ch->channelIndex >= playbackHardwareChannels) continue;
				float* ptr = channelPtr(&outAreas[ch->channelIndex], outOffset);
				if (!ptr) continue;

				const size_t missing = realtime::renderOutput(
					ch->channel,
					ptr,
					backendFrames,
					outputResampleScratch.get(),
					constants::MAX_FIFO_SIZE
				);

				if (missing > 0) {
					underrunCount.fetch_add(1, std::memory_order_relaxed);
					outputMissingFrames.fetch_add(missing, std::memory_order_relaxed);
				}
			}

			if (readyOut) {
				snd_pcm_sframes_t committed = snd_pcm_mmap_commit(playback, outOffset, backendFrames);
				if (committed < 0) {
					handleRecoverableXrun(playback, static_cast<int>(committed));
				}
			}
			if (readyIn) {
				snd_pcm_sframes_t committed = snd_pcm_mmap_commit(capture, inOffset, backendFrames);
				if (committed < 0) {
					handleRecoverableXrun(capture, static_cast<int>(committed));
				}
			}
		}

		void handleRecoverableXrun(snd_pcm_t* handle, int errCode = -EPIPE) {
			if (!handle) return;
			const int recoverErr = snd_pcm_recover(handle, errCode, 1);
			if (recoverErr < 0) {
				logAlsaError("snd_pcm_recover", recoverErr);
			}
			xrunCount.fetch_add(1, std::memory_order_relaxed);
		}

		void refreshPollDescriptors() {
			const size_t total = static_cast<size_t>(std::max(playbackDescriptorCount, 0) + std::max(captureDescriptorCount, 0));
			pollDescriptors.assign(total, pollfd {});

			if (playback && playbackDescriptorCount > 0) {
				snd_pcm_poll_descriptors(playback, pollDescriptors.data(), playbackDescriptorCount);
			}
			if (capture && captureDescriptorCount > 0) {
				snd_pcm_poll_descriptors(capture, pollDescriptors.data() + playbackDescriptorCount, captureDescriptorCount);
			}
		}

		Result failAndClose(snd_pcm_t** handle, int err, Error error, const char* what) {
			logAlsaError(what, err);
			if (*handle) {
				snd_pcm_close(*handle);
				*handle = nullptr;
			}
			return Result { error, snd_strerror(err) };
		}

	private:
		snd_pcm_t* playback = nullptr;
		snd_pcm_t* capture = nullptr;

		std::string playbackDevice;
		std::string captureDevice;

		std::thread worker;
		std::atomic<bool> workerStop = false;

		std::vector<pollfd> pollDescriptors;
		std::vector<EnumeratedChannel> discoveredChannels;
		int playbackDescriptorCount = 0;
		int captureDescriptorCount = 0;

		std::unique_ptr<AlsaChannelHandle[]> openedChannels;
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

		uint32_t playbackHardwareChannels = 0;
		uint32_t captureHardwareChannels = 0;
	};
}
