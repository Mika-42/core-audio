module;
#include <string>
#include <vector>
#include <format>
#include <alsa/asoundlib.h>
export module audio.alsa;
export import audio.abstract_core; //TODO rm export keyword

namespace mka::audio {
	
	// helpers
	
	SampleFormat fromALSA(snd_pcm_format_t fmt) {
		switch(fmt) {
			case SND_PCM_FORMAT_S16_LE:			return SampleFormat::Int16;
			case SND_PCM_FORMAT_S24_LE:			return SampleFormat::Int24;
			case SND_PCM_FORMAT_S32_LE:			return SampleFormat::Int32;
			case SND_PCM_FORMAT_FLOAT_LE:		return SampleFormat::Float32;
			case SND_PCM_FORMAT_FLOAT64_LE:		return SampleFormat::Float64;
			default:							return SampleFormat::Invalid;
		}
	}

	snd_pcm_format_t toALSA(SampleFormat fmt) {
		switch(fmt) {
			case SampleFormat::Int16:			return SND_PCM_FORMAT_S16_LE;
			case SampleFormat::Int24:			return SND_PCM_FORMAT_S24_LE;
			case SampleFormat::Int32:			return SND_PCM_FORMAT_S32_LE;
			case SampleFormat::Float32:			return SND_PCM_FORMAT_FLOAT_LE;
			case SampleFormat::Float64:			return SND_PCM_FORMAT_FLOAT64_LE;
			default:							return SND_PCM_FORMAT_UNKNOWN;
		}
	}

	 void getChannels(snd_pcm_t* pcm, size_t& channels) {
		snd_pcm_hw_params_t* params = nullptr;

		if(snd_pcm_hw_params_malloc(&params) < 0) {
			return;
		}

		if(snd_pcm_hw_params_any(pcm, params) < 0) {
			snd_pcm_hw_params_free(params);
			return;
		}

		unsigned int maxChannels = 0;

		if(snd_pcm_hw_params_get_channels_max(params, &maxChannels) < 0)
		{
			snd_pcm_hw_params_free(params);
			return;
		}

		channels = maxChannels;

		snd_pcm_hw_params_free(params);
    }

	void getFormats(snd_pcm_t* pcm, std::vector<SampleFormat>& formats) {
        snd_pcm_hw_params_t* params = nullptr;

        if(snd_pcm_hw_params_malloc(&params) < 0) {
			return;
		}

        if(snd_pcm_hw_params_any(pcm, params) < 0) {
            snd_pcm_hw_params_free(params);
            return;
        }

        static constexpr snd_pcm_format_t alsaFormats[] = {
            SND_PCM_FORMAT_S16_LE,
            SND_PCM_FORMAT_S24_LE,
            SND_PCM_FORMAT_S32_LE,
            SND_PCM_FORMAT_FLOAT_LE,
            SND_PCM_FORMAT_FLOAT64_LE
        };

        for (snd_pcm_format_t format : alsaFormats)
        {
            if (snd_pcm_hw_params_test_format(pcm, params, format) < 0) {
                continue;
            }

            const SampleFormat sampleFormat = fromALSA(format);

            if (sampleFormat != SampleFormat::Invalid) {
                formats.push_back(sampleFormat);
			}
        }

        snd_pcm_hw_params_free(params);
    }

	void getSampleRates(snd_pcm_t* pcm, std::vector<size_t>& rates) {
        snd_pcm_hw_params_t* params = nullptr;

        if(snd_pcm_hw_params_malloc(&params) < 0) {
            return;
		}

        if(snd_pcm_hw_params_any(pcm, params) < 0) {
            snd_pcm_hw_params_free(params);
            return;
        }

        for (size_t rate : Backend::SUPPORTED_SAMPLE_RATES)
        {
            if (snd_pcm_hw_params_test_rate(pcm, params, static_cast<unsigned int>(rate), 0) >= 0) {
                rates.push_back(rate);
            }
        }

        snd_pcm_hw_params_free(params);
    }
	
	void getBufferSizes(snd_pcm_t* pcm, std::vector<size_t>& bufferSizes) {
        snd_pcm_hw_params_t* params = nullptr;

        if(snd_pcm_hw_params_malloc(&params) < 0) {
            return;
		}

        if(snd_pcm_hw_params_any(pcm, params) < 0) {
            snd_pcm_hw_params_free(params);
            return;
        }

		for(size_t size : Backend::SUPPORTED_BUFFER_SIZES) {
			if(snd_pcm_hw_params_test_buffer_size(pcm, params, size) == 0) {
				bufferSizes.push_back(size);
			}
		}

		snd_pcm_hw_params_free(params);
    }

	bool hasPCM(snd_ctl_t* ctl, int device, snd_pcm_stream_t stream) {
		snd_pcm_info_t* info = nullptr;

		if (snd_pcm_info_malloc(&info) < 0) {
			return false;
		}

		snd_pcm_info_set_device(info, device);
		snd_pcm_info_set_subdevice(info, 0);
		snd_pcm_info_set_stream(info, stream);	
	
		const bool result = snd_ctl_pcm_info(ctl, info) >= 0;

		snd_pcm_info_free(info);

	    return result;
	}

	std::string getPCMName(snd_ctl_t* ctl, int device, snd_pcm_stream_t stream) {
    
		snd_pcm_info_t* info = nullptr;

		if(snd_pcm_info_malloc(&info) < 0) {
			return {};
		}

		snd_pcm_info_set_device(info, device);
	    snd_pcm_info_set_subdevice(info, 0);
	    snd_pcm_info_set_stream(info, stream);

		if(snd_ctl_pcm_info(ctl, info) < 0) {
			snd_pcm_info_free(info);
			return {};
		}

		const char* name = snd_pcm_info_get_name(info);

		std::string result = name ? name : "";

		snd_pcm_info_free(info);

		return result;
	}
	
	std::string getCardName(snd_ctl_t* ctl) {
		snd_ctl_card_info_t* info = nullptr;

		if(snd_ctl_card_info_malloc(&info) < 0) {
			return {};
		}

		if(snd_ctl_card_info(ctl, info) < 0) {
			snd_ctl_card_info_free(info);
			return {};
		}

		const char* name = snd_ctl_card_info_get_name(info);

		std::string result = name ? name : "";

		snd_ctl_card_info_free(info);

		return result;
	}

	export class ALSA : public Backend {
		public:

			virtual std::vector<DeviceDescriptor> getDevices() {
				std::vector<DeviceDescriptor> devices;

				int card = -1;


				if(snd_card_next(&card) < 0) {
					return devices;
				}

				while(card >= 0) {
					const std::string cardId = std::format("hw:{}", card);
					
					snd_ctl_t* ctl = nullptr;

					if(snd_ctl_open(&ctl, cardId.c_str(), 0) < 0) {
						if(snd_card_next(&card) < 0) {
							break;
						}

						continue;
					}

					const std::string cardName = getCardName(ctl);

					int pcmDevice = -1;
					while(true) {
						if(snd_ctl_pcm_next_device(ctl, &pcmDevice) < 0) {
							break;
						}

						if(pcmDevice < 0) {
							break;
						}

						const bool hasInput = hasPCM(ctl, pcmDevice, SND_PCM_STREAM_CAPTURE);
						const bool hasOutput = hasPCM(ctl, pcmDevice, SND_PCM_STREAM_PLAYBACK);

						if (!hasInput && !hasOutput) {
							continue;
						}

						const auto streamType = hasOutput ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
					
						// create a desriptor
						DeviceDescriptor info = {};
						info.id = std::format("hw:{},{}", card, pcmDevice);
						info.name = getPCMName(ctl, pcmDevice, streamType);
						
						if (info.name.empty()) {
							info.name = cardName;
						}

						if (info.name.empty()) {
							info.name = info.id;
						}

						if(hasOutput) {

							// open device
							snd_pcm_t* pcm = nullptr;
							
							if (snd_pcm_open(&pcm, info.id.c_str(), SND_PCM_STREAM_PLAYBACK, 0) >= 0) {
								getFormats(pcm, info.sampleFormats);
								getSampleRates(pcm, info.sampleRates);
								getBufferSizes(pcm, info.bufferSizes);
								getChannels(pcm, info.outputChannels);

								snd_pcm_close(pcm);
							}
						}

						if(hasInput) {
							snd_pcm_t* pcm = nullptr;

							if(snd_pcm_open(&pcm, info.id.c_str(), SND_PCM_STREAM_CAPTURE, 0) >= 0) {
								
								if(!hasOutput) {
									getFormats(pcm, info.sampleFormats);
									getSampleRates(pcm, info.sampleRates);
									getBufferSizes(pcm, info.bufferSizes);
								}
								
								getChannels(pcm, info.inputChannels);
								snd_pcm_close(pcm);
							}
						}
						
						devices.push_back(std::move(info));
					}
				
					snd_ctl_close(ctl);

					if(snd_card_next(&card) < 0) {
						break;
					}
				}

				return devices;
			}
	
			// open device with a configuration, in case of fail, do not negotiate. only fail
			virtual bool open(const DeviceConfig& /*cfg*/) {
				return false;
			};

			virtual bool close() {

				return false;
			}
			
			virtual bool start() {

				return false;
			}

			virtual bool stop() {

				return false;
			}
		private:

	};
}
