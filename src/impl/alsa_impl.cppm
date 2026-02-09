module;
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <alsa/asoundlib.h>

#define ALSA_CHECK(_call_or_value, _error_code)								\
	do {																	\
		int _alsa_retcode = (_call_or_value);								\
		if(_alsa_retcode < 0) {												\
			return Result { _error_code, snd_strerror(_alsa_retcode) };		\
		}																	\
	} while(0);

export module audio.alsa;
export import audio.block;
export import audio.config;

import audio.error;
import audio.abstract_core;

export namespace mka::audio {

	class ALSA final: public AbstractCoreAudio {

	public:
		ALSA() = default;
		
		Result open(const Config& config) override {
			this->config = config;

			// open the device
			int err = snd_pcm_open(&playback, this->config.name.c_str(), 
					SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
			
			ALSA_CHECK(err,	Error::DeviceOpenFailed);

			// setup hardware
			snd_pcm_hw_params_malloc(&hardwareParameter);
			snd_pcm_hw_params_any(playback, hardwareParameter);

			Result ret = setupHardwareParameter(playback, hardwareParameter);
			if(!ret.ok()) {
				return ret;
			}

			err = snd_pcm_hw_params(playback, hardwareParameter);

			ALSA_CHECK(err,	Error::HardwareSetupFailed);
			
			// poll descriptors
			unsigned int count = snd_pcm_poll_descriptors_count(playback);

			if(count <= 0) {
				return Result{Error::PollSetupFailed, "Invalid poll descriptor count"};
			}

			pfds.resize(count);
			
			err = snd_pcm_poll_descriptors(playback, pfds.data(), count);
			ALSA_CHECK(err, Error::PollDescriptorsFailed);

			return mka::audio::Ok;
		}
		
		Result close() override {
			if(playback) {
				snd_pcm_drain(playback);
				snd_pcm_close(playback);
				playback = nullptr;
			}
			
			if(hardwareParameter) {
				snd_pcm_hw_params_free(hardwareParameter);
				hardwareParameter = nullptr;
			}
			return mka::audio::Ok;
		}

	protected:

		void run() override {
			std::vector<float*> outPtrs(config.channels, nullptr);
			
			snd_pcm_prepare(playback);
			snd_pcm_start(playback);

			while(running.load(std::memory_order_acquire)) {

				snd_pcm_sframes_t avail = snd_pcm_avail_update(playback);
				if (avail <= 0) continue;

				const snd_pcm_channel_area_t* areas = nullptr;
				snd_pcm_uframes_t offset = 0;
				snd_pcm_uframes_t frames = avail < config.bufferSize ? avail : config.bufferSize;

				int err = snd_pcm_mmap_begin(playback, &areas, &offset, &frames);
			   
				if(err < 0) {
					if (snd_pcm_recover(playback, err, 1) < 0) { 
						break;
					}
					continue;
				}
				// store pointers in AudioBlock view 		
			
				for(uint32_t channel = 0; channel < config.channels; ++channel) {
					outPtrs[channel] = reinterpret_cast<float*>(
							static_cast<char*>(areas[channel].addr) 
							+ (areas[channel].first / 8) + offset * (areas[channel].step / 8)
					);
				}

				mka::audio::Block block {
					.samplerate = config.samplerate,
					.channels = config.channels,
					.frames = static_cast<uint32_t>(frames),
					.out = outPtrs.data(),
					.in = nullptr,
				};

				if(callback) {
					callback(block);
				} else {
					//fill buffer with 0 if callback is not set properly
					for(uint32_t ch = 0; ch < block.channels; ++ch) {
				        std::memset(block.out[ch], 0, block.frames * sizeof(float));
					}
				}

				err = snd_pcm_mmap_commit(playback, offset, frames);

				if(err < 0 || err != frames) {
					snd_pcm_recover(playback, err, 1);
				}
			}

			if(playback) {
				snd_pcm_drop(playback);
				snd_pcm_prepare(playback);
			}
		}

	private:
		Result setupHardwareParameter(snd_pcm_t* pcm, snd_pcm_hw_params_t* hw) {
		
			unsigned int		rate		= config.samplerate;
			snd_pcm_uframes_t	bufferSize	= config.bufferSize * 4;
			snd_pcm_uframes_t	periodSize	= config.bufferSize;	
			snd_pcm_format_t	realFormat;

			int err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			snd_pcm_hw_params_get_format(hw, &realFormat);

			if(realFormat != SND_PCM_FORMAT_FLOAT_LE) {
				return { Error::SetupHardwareParameterFailed, "Device does not support float" };
			}

			err = snd_pcm_hw_params_set_channels(pcm, hw, config.channels);		
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);			
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufferSize);		
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);

			err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &periodSize, nullptr);	
			ALSA_CHECK(err, Error::SetupHardwareParameterFailed);
			
			return mka::audio::Ok;
		}

	private:

		snd_pcm_t*				playback			 = nullptr;
		snd_pcm_hw_params_t*	hardwareParameter	 = nullptr;
		std::vector<pollfd>		pfds;
	};
}
