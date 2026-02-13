module;
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <alsa/asoundlib.h>

#include <sys/mman.h>

inline void logAlsaError(const char* what, int code) {
	std::fprintf(stderr, "[ALSA] %s failed: %s (%d)\n", what, snd_strerror(code), code);
}

#define ALSA_CHECK(_call_or_value, _error_code)	do {							\
	int _alsa_retcode = (_call_or_value);										\
	if(_alsa_retcode < 0) {														\
		logAlsaError(#_call_or_value, _alsa_retcode);							\
		return mka::audio::Result { _error_code, snd_strerror(_alsa_retcode) };	\
	}																			\
} while(0);

#define ALSA_LOG_ERROR(_call_or_value)	do {						\
	int _alsa_retcode = (_call_or_value);							\
	if(_alsa_retcode < 0) {											\
		logAlsaError(#_call_or_value, _alsa_retcode);				\
	}																\
} while(0);

export module audio.alsa;
export import audio.block;
export import audio.config;

import audio.error;
import audio.abstract_core;

//---- alsa wrapper ----//
snd_pcm_format_t select_format(mka::audio::Format fmt) {
    switch(fmt) {
        case mka::audio::Format::Int16:   return SND_PCM_FORMAT_S16_LE;
        case mka::audio::Format::Int24:   return SND_PCM_FORMAT_S24_LE;
        case mka::audio::Format::Int32:   return SND_PCM_FORMAT_S32_LE;
        case mka::audio::Format::Float32: return SND_PCM_FORMAT_FLOAT_LE;
        case mka::audio::Format::Float64: return SND_PCM_FORMAT_FLOAT64_LE;
    }
    return SND_PCM_FORMAT_FLOAT_LE;
}

mka::audio::Result setup_pcm(
		snd_pcm_t**			handle, 
		const char*			device_name, 
		snd_pcm_stream_t	stream_mode, 
		uint32_t			channels,
		int&				descriptor_count,
		uint32_t&			samplerate,
		uint32_t			buffer_size,
		mka::audio::Format	fmt) {

	// open the output device
	ALSA_CHECK(snd_pcm_open(handle, device_name, stream_mode, SND_PCM_NONBLOCK), mka::audio::Error::DeviceOpenFailed);
				
	// setup output hardware
	snd_pcm_hw_params_t* hw = nullptr;
	snd_pcm_hw_params_alloca(&hw);
	ALSA_CHECK(snd_pcm_hw_params_any(*handle, hw), mka::audio::Error::HardwareSetupFailed);
	snd_pcm_uframes_t	bufferSize	= buffer_size * 2;
	snd_pcm_uframes_t	periodSize	= buffer_size;	
	snd_pcm_format_t	desired_format = select_format(fmt);
	snd_pcm_format_t	real_format = {};

	ALSA_CHECK(snd_pcm_hw_params_set_access(*handle, hw, SND_PCM_ACCESS_MMAP_NONINTERLEAVED), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_hw_params_set_format(*handle, hw, desired_format), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_hw_params_get_format(hw, &real_format), mka::audio::Error::SetupHardwareParameterFailed);

	if(real_format != desired_format) {
		return mka::audio::Result { mka::audio::Error::SetupHardwareParameterFailed, "Audio format is not supported" };
	}

	ALSA_CHECK(snd_pcm_hw_params_set_channels(*handle, hw, channels), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_hw_params_set_rate_near(*handle, hw, &samplerate, nullptr), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_hw_params_set_buffer_size_near(*handle, hw, &bufferSize), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_hw_params_set_period_size_near(*handle, hw, &periodSize, nullptr), mka::audio::Error::SetupHardwareParameterFailed);	
	ALSA_CHECK(snd_pcm_hw_params(*handle, hw), mka::audio::Error::HardwareSetupFailed);
	// setup output software	
	snd_pcm_sw_params_t* sw = nullptr;
	snd_pcm_sw_params_alloca(&sw);

	ALSA_CHECK(snd_pcm_sw_params_current(*handle, sw), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_sw_params_set_start_threshold(*handle, sw, buffer_size), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_sw_params_set_avail_min(*handle, sw, buffer_size), mka::audio::Error::SetupHardwareParameterFailed);
	ALSA_CHECK(snd_pcm_sw_params(*handle, sw), mka::audio::Error::SetupHardwareParameterFailed);
	
	// poll descriptors count
	descriptor_count = snd_pcm_poll_descriptors_count(*handle);

	if(descriptor_count < 0) {
		logAlsaError("snd_pcm_poll_descriptors_count", descriptor_count);
		return mka::audio::Result {mka::audio::Error::PollSetupFailed, snd_strerror(descriptor_count)};
	}

	return mka::audio::Ok;
}

bool begin(
		snd_pcm_t* handle,
		const snd_pcm_channel_area_t **areas,
		snd_pcm_uframes_t buffer_size,
		uint32_t channels,
		snd_pcm_uframes_t& offset,
		snd_pcm_uframes_t& frames, std::span<float*> ptrs) {

	if(channels == 0) {
		return false;
	}

	snd_pcm_sframes_t avail = snd_pcm_avail_update(handle);

	if(avail < 0) {
		ALSA_LOG_ERROR(snd_pcm_recover(handle, avail, 1));
		return false;
	}

	frames = std::min(static_cast<snd_pcm_uframes_t>(avail), buffer_size);

	if(frames == 0) {
		return false;
	}

	int err = snd_pcm_mmap_begin(handle, areas, &offset, &frames);

	if(err < 0) {
		ALSA_LOG_ERROR(snd_pcm_recover(handle, err, 1));
		return false;
	}

	for(uint32_t channel = 0; channel < channels; ++channel) {
		ptrs[channel] = reinterpret_cast<float*>(
			static_cast<char*>(areas[channel]->addr)
			+ (areas[channel]->first / 8)
			+ offset * (areas[channel]->step / 8)
		);
	}

	return true;
}

//---------------------//
export namespace mka::audio {

	class ALSA final: public AbstractCoreAudio {
	
	public:
		ALSA() = default;
		
		Result open(const Config& cfg) override {
			config = cfg;
			
			// poll descriptors count
			
			if(config.outChannels > 0) {

				Result ret = setup_pcm(
						&playback, 
						config.name.c_str(),
						SND_PCM_STREAM_PLAYBACK,
						config.outChannels,
						outCount,
						config.samplerate,
						config.bufferSize, 
						config.audioFormat
				);
				
				if(!ret.ok()) {
					return ret;
				}
			}

			if(config.inChannels > 0) {
				Result ret = setup_pcm(
						&capture,
						config.name.c_str(), 
						SND_PCM_STREAM_CAPTURE, 
						config.inChannels, 
						inCount, 
						config.samplerate, 
						config.bufferSize, 
						config.audioFormat
				);

				if(!ret.ok()) {
					return ret;
				}
			}

			const size_t totalCount = outCount + inCount;

			if(totalCount <= 0) {
				return Result{Error::PollSetupFailed, "Invalid poll descriptor count"};
			}

			pfds.resize(totalCount);
			
			if(config.outChannels > 0) {
				// poll descriptors out
				ALSA_CHECK(snd_pcm_poll_descriptors(playback, pfds.data(), outCount), Error::PollDescriptorsFailed);
			}

			if(config.inChannels > 0) { 
				// poll descriptors in
				ALSA_CHECK(snd_pcm_poll_descriptors(capture, pfds.data() + outCount, inCount), Error::PollDescriptorsFailed);
			}

			outPtrs.resize(config.outChannels, nullptr);
			inPtrs.resize(config.inChannels, nullptr);
			
			mlockall(MCL_CURRENT | MCL_FUTURE);			
			
			return mka::audio::Ok;
		}
		
		Result close() override {
			
			munlockall();

			if(playback) {
				ALSA_LOG_ERROR(snd_pcm_drain(playback));
				ALSA_LOG_ERROR(snd_pcm_close(playback));
				playback = nullptr;
			}
			
			if(capture) {
				ALSA_LOG_ERROR(snd_pcm_drain(capture));
				ALSA_LOG_ERROR(snd_pcm_close(capture));
				capture = nullptr;
			}
			return mka::audio::Ok;
		}

	protected:

		void run() override {

			const bool hasPlayback = config.outChannels > 0;
			const bool hasCapture  = config.inChannels  > 0;
			bool playbackStarted = false;
	
			const snd_pcm_channel_area_t* outAreas = nullptr;
			const snd_pcm_channel_area_t* inAreas  = nullptr;
			snd_pcm_uframes_t framesPlayback = 0;
			snd_pcm_uframes_t framesCapture  = 0;
			snd_pcm_uframes_t playbackOffset = 0;
			snd_pcm_uframes_t captureOffset  = 0;

			if(hasPlayback) {
		        ALSA_LOG_ERROR(snd_pcm_prepare(playback));
		        playbackStarted = false;
		    }

		    if(hasCapture) {
		        ALSA_LOG_ERROR(snd_pcm_prepare(capture));
		        ALSA_LOG_ERROR(snd_pcm_start(capture));
		    }

			while(running.load(std::memory_order_acquire)) {

				if(poll(pfds.data(), pfds.size(), -1) <= 0) {
					continue;
				}

				unsigned short revOut = 0, revIn = 0;

				if(hasPlayback) {
					ALSA_LOG_ERROR(snd_pcm_poll_descriptors_revents(playback, pfds.data(), outCount, &revOut));
				}

				if(hasCapture) {
					ALSA_LOG_ERROR(snd_pcm_poll_descriptors_revents(capture, pfds.data() + outCount, inCount, &revIn));
				}

				if((revOut | revIn) & POLLERR) {
					if(hasPlayback) {
						ALSA_LOG_ERROR(snd_pcm_recover(playback, -EPIPE, 1));
					}

					if(hasCapture) {
						ALSA_LOG_ERROR(snd_pcm_recover(capture,  -EPIPE, 1));
					}

					continue;
				}


				bool readyPlayback = hasPlayback && begin(playback, &outAreas, config.bufferSize, config.outChannels, playbackOffset, framesPlayback, outPtrs);
				bool readyCapture  = hasCapture  && (revIn  & POLLIN) && begin(capture, &inAreas, config.bufferSize, config.inChannels, captureOffset, framesCapture, inPtrs);

				if(!readyPlayback && !readyCapture) {
					continue;
				}
				
				snd_pcm_uframes_t frames = config.bufferSize;

				if(readyPlayback) frames = std::min(frames, framesPlayback);
				if(readyCapture)  frames = std::min(frames, framesCapture);

				if(frames == 0) continue;

				mka::audio::Block block {
					.samplerate  = config.samplerate,
					.outChannels = readyPlayback ? config.outChannels : 0,
				    .inChannels  = readyCapture  ? config.inChannels  : 0,
					.frames      = static_cast<uint32_t>(frames),
					.out         = readyPlayback ? outPtrs.data() : nullptr,
					.in          = readyCapture  ? inPtrs.data()  : nullptr
		        };

				if(callback) {
					callback(block);
				} else if(readyPlayback) {
					for(uint32_t ch = 0; ch < block.outChannels; ++ch) {
		                std::memset(block.out[ch], 0, block.frames * sizeof(float));
					}
				}

				if(readyPlayback) {
					snd_pcm_sframes_t committed = snd_pcm_mmap_commit(playback, playbackOffset, frames);
					if(committed < 0) {
						logAlsaError("snd_pcm_mmap_commit(playback)", static_cast<int>(committed));
						ALSA_LOG_ERROR(snd_pcm_recover(playback, static_cast<int>(committed), 1));
						playbackStarted = false;
						continue;
					}
					
					if(static_cast<snd_pcm_uframes_t>(committed) != frames) {
						std::fprintf(stderr, "[ALSA] short playback commit: requested=%lu committed=%ld\n",
							static_cast<unsigned long>(frames),
							static_cast<long>(committed));
					}

					if(!playbackStarted) {
						snd_pcm_state_t state = snd_pcm_state(playback);
						if(state == SND_PCM_STATE_PREPARED) {
							int startErr = snd_pcm_start(playback);
							if(startErr < 0) {
								logAlsaError("snd_pcm_start(playback)", startErr);
								ALSA_LOG_ERROR(snd_pcm_recover(playback, startErr, 1));
							} else {
								playbackStarted = true;
							}
						} else if(state == SND_PCM_STATE_RUNNING) {
							playbackStarted = true;
						}
					}
				}

				if(readyCapture) {
					snd_pcm_sframes_t committed = snd_pcm_mmap_commit(capture, captureOffset, frames);
					if(committed < 0) {
						logAlsaError("snd_pcm_mmap_commit(capture)", static_cast<int>(committed));
						ALSA_LOG_ERROR(snd_pcm_recover(capture, static_cast<int>(committed), 1));
					}
				}
			}

			if(hasPlayback) {
				ALSA_LOG_ERROR(snd_pcm_drop(playback));
			}
		
			if(hasCapture) {
				ALSA_LOG_ERROR(snd_pcm_drop(capture));
			}
		}


	private:

		snd_pcm_t*			playback = nullptr;
		snd_pcm_t*			capture	 = nullptr;

		std::vector<pollfd>	pfds;
		std::vector<float*>	outPtrs;
		std::vector<float*>	inPtrs;

		int					outCount = 0;
		int					inCount	 = 0;	
	};
}
