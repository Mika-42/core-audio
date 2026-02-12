module;
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <alsa/asoundlib.h>

static inline void logAlsaError(const char* what, int code) {
	std::fprintf(stderr, "[ALSA] %s failed: %s (%d)\n", what, snd_strerror(code), code);
}

#define ALSA_CHECK(_call_or_value, _error_code)	do {		int _alsa_retcode = (_call_or_value);		if(_alsa_retcode < 0) {			logAlsaError(#_call_or_value, _alsa_retcode);			return Result { _error_code, snd_strerror(_alsa_retcode) };		}	} while(0);

#define ALSA_LOG_ERROR(_call_or_value)	do {		int _alsa_retcode = (_call_or_value);		if(_alsa_retcode < 0) {			logAlsaError(#_call_or_value, _alsa_retcode);		}	} while(0);

export module audio.alsa;
export import audio.block;
export import audio.config;

import audio.error;
import audio.abstract_core;


export namespace mka::audio {

	class ALSA final: public AbstractCoreAudio {
	
	public:
		ALSA() = default;
		
		Result open(const Config& cfg) override {
			config = cfg;
			
			// poll descriptors count
			
			if(config.outChannels > 0) {
				
				// open the output device
				int err = snd_pcm_open(&playback, config.name.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
				ALSA_CHECK(err,	Error::DeviceOpenFailed);
			
				// setup output hardware
				ALSA_CHECK(snd_pcm_hw_params_malloc(&playbackParameter), Error::HardwareSetupFailed);
				ALSA_CHECK(snd_pcm_hw_params_any(playback, playbackParameter), Error::HardwareSetupFailed);

				Result ret = setupHardwareParameter(playback, playbackParameter, config.outChannels);
				if(!ret.ok()) {
					return ret;
				}

				err = snd_pcm_hw_params(playback, playbackParameter);
				ALSA_CHECK(err,	Error::HardwareSetupFailed);

				Result swRet = setupSoftwareParameter(playback);
        
				if(!swRet.ok()) {
					return swRet;
				}

				// poll descriptors count
				outCount = snd_pcm_poll_descriptors_count(playback);
				if(outCount < 0) {
					logAlsaError("snd_pcm_poll_descriptors_count(playback)", static_cast<int>(outCount));
					return Result{Error::PollSetupFailed, snd_strerror(static_cast<int>(outCount))};
				}
			}

			if(config.inChannels > 0) {
				// open the input device
				int err = snd_pcm_open(&capture, config.name.c_str(), SND_PCM_STREAM_CAPTURE, 0);
				ALSA_CHECK(err,	Error::DeviceOpenFailed);
				
				// setup input hardware
				ALSA_CHECK(snd_pcm_hw_params_malloc(&captureParameter), Error::HardwareSetupFailed);
				ALSA_CHECK(snd_pcm_hw_params_any(capture, captureParameter), Error::HardwareSetupFailed);

				Result ret = setupHardwareParameter(capture, captureParameter, config.inChannels);
				if(!ret.ok()) {
					return ret;
				}
				err = snd_pcm_hw_params(capture, captureParameter);
				ALSA_CHECK(err,	Error::HardwareSetupFailed);

				Result swRet = setupSoftwareParameter(capture);
				
				if(!swRet.ok()) {
					return swRet;
				}

				// poll descriptors count
				inCount = snd_pcm_poll_descriptors_count(capture);
				if(inCount < 0) {
					logAlsaError("snd_pcm_poll_descriptors_count(capture)", static_cast<int>(inCount));
					return Result{Error::PollSetupFailed, snd_strerror(static_cast<int>(inCount))};
				}
			}

			if(outCount + inCount <= 0) {
				return Result{Error::PollSetupFailed, "Invalid poll descriptor count"};
			}

			pfds.resize(static_cast<size_t>(outCount + inCount));
			
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
			
			return mka::audio::Ok;
		}
		
		Result close() override {
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

			if(playbackParameter) {
				snd_pcm_hw_params_free(playbackParameter);
				playbackParameter = nullptr;
			}

			if(captureParameter) {
				snd_pcm_hw_params_free(captureParameter);
				captureParameter = nullptr;
			}

			return mka::audio::Ok;
		}

	protected:

		void run() override {

			const bool hasPlayback = config.outChannels > 0;
			const bool hasCapture  = config.inChannels  > 0;

			if(hasPlayback) {
		        ALSA_LOG_ERROR(snd_pcm_prepare(playback));
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

				snd_pcm_uframes_t framesPlayback = 0;
				snd_pcm_uframes_t framesCapture  = 0;

				bool readyPlayback = hasPlayback && (revOut & POLLOUT) && beginPlayback(framesPlayback);
				bool readyCapture  = hasCapture  && (revIn  & POLLIN) && beginCapture(framesCapture);

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
						continue;
					}

					if(static_cast<snd_pcm_uframes_t>(committed) != frames) {
						std::fprintf(stderr, "[ALSA] short playback commit: requested=%lu committed=%ld\n",
							static_cast<unsigned long>(frames),
							static_cast<long>(committed));
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
		Result setupSoftwareParameter(snd_pcm_t* pcm) {

			snd_pcm_sw_params_t* sw = nullptr;
			snd_pcm_sw_params_alloca(&sw);

			int err = snd_pcm_sw_params_current(pcm, sw);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_sw_params_set_start_threshold(pcm, sw, config.bufferSize);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_sw_params_set_avail_min(pcm, sw, config.bufferSize);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_sw_params(pcm, sw);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			return mka::audio::Ok;
		}

		Result setupHardwareParameter(snd_pcm_t* pcm, snd_pcm_hw_params_t* hw, uint32_t channels) {
		
			unsigned int		rate		= config.samplerate;
			snd_pcm_uframes_t	bufferSize	= config.bufferSize * 2;
			snd_pcm_uframes_t	periodSize	= config.bufferSize;	
			snd_pcm_format_t	realFormat;

			int err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			ALSA_CHECK(snd_pcm_hw_params_get_format(hw, &realFormat), Error::SetupHardwareParameterFailed);

			if(realFormat != SND_PCM_FORMAT_FLOAT_LE) {
				return { Error::SetupHardwareParameterFailed, "Device does not support float" };
			}

			err = snd_pcm_hw_params_set_channels(pcm, hw, channels);		
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);			
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufferSize);		
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &periodSize, nullptr);	
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);
				
			return mka::audio::Ok;
		}

		bool beginPlayback(snd_pcm_uframes_t& frames) {

			if(config.outChannels == 0) {
				return false;		
			}

			snd_pcm_sframes_t avail = snd_pcm_avail_update(playback);
			
			if(avail < 0) {
				ALSA_LOG_ERROR(snd_pcm_recover(playback, avail, 1));
				return false;
			}

			frames = std::min(
					static_cast<snd_pcm_uframes_t>(avail), 
					static_cast<snd_pcm_uframes_t>(config.bufferSize)
			);

			int err = snd_pcm_mmap_begin(playback, &outAreas, &playbackOffset, &frames);
		   
			if(err < 0) {
				ALSA_LOG_ERROR(snd_pcm_recover(playback, err, 1));
				return false;
			}

			for(uint32_t channel = 0; channel < config.outChannels; ++channel) {
				outPtrs[channel] = reinterpret_cast<float*>(
					static_cast<char*>(outAreas[channel].addr) 
					+ (outAreas[channel].first / 8) 
					+ playbackOffset * (outAreas[channel].step / 8)
				);
			}

			playbackFrames = frames;
			return true;
		}

		bool beginCapture(snd_pcm_uframes_t& frames) {

			if(config.inChannels == 0) {
				return false;
			}
			
			snd_pcm_sframes_t avail = snd_pcm_avail_update(capture);
			
			if(avail < 0) {
				ALSA_LOG_ERROR(snd_pcm_recover(capture, avail, 1));
				return false;
			}

			frames = std::min(
					static_cast<snd_pcm_uframes_t>(avail), 
					static_cast<snd_pcm_uframes_t>(config.bufferSize)
			);

			int err = snd_pcm_mmap_begin(capture, &inAreas, &captureOffset, &frames);
		   
			if(err < 0) {
				ALSA_LOG_ERROR(snd_pcm_recover(capture, err, 1));
				return false;
			}

			for(uint32_t channel = 0; channel < config.inChannels; ++channel) {
				inPtrs[channel] = reinterpret_cast<float*>(
					static_cast<char*>(inAreas[channel].addr) 
					+ (inAreas[channel].first / 8) 
					+ captureOffset * (inAreas[channel].step / 8)
				);
			}
			
			captureFrames = frames;

		    return true;
		}

		void processBlock(snd_pcm_uframes_t frames) {
		
		}

		void commit() {
			ALSA_LOG_ERROR(snd_pcm_mmap_commit(playback, playbackOffset, playbackFrames));
			
			if(config.inChannels) {
				ALSA_LOG_ERROR(snd_pcm_mmap_commit(capture, captureOffset, captureFrames));
			}
		}

	private:

		snd_pcm_t*				playback			 = nullptr;
		snd_pcm_t*				capture				 = nullptr;
		snd_pcm_hw_params_t*	playbackParameter	 = nullptr;
		snd_pcm_hw_params_t*	captureParameter	 = nullptr;
		std::vector<pollfd>		pfds;
		std::vector<float*>		outPtrs;
		std::vector<float*>		inPtrs;

		int outCount = 0;
		int inCount = 0;	

		const snd_pcm_channel_area_t* outAreas = nullptr;
		const snd_pcm_channel_area_t* inAreas  = nullptr;

		snd_pcm_uframes_t playbackOffset = 0;
		snd_pcm_uframes_t captureOffset  = 0;
		snd_pcm_uframes_t playbackFrames = 0;
		snd_pcm_uframes_t captureFrames  = 0;
	};
}
