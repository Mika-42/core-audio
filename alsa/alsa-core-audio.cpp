#include "alsa-core-audio.hpp"

std::expected<void, std::string> ALSA::open(
	const Device& inputDevice, const Device& outputDevice)
{
	if(m_playbackHandle || m_captureHandle)
	{
		return std::unexpected("ALSA handles already open");	
	}

	int err = 0;
 	
	if(outputDevice.type == Output)
	{
		err = snd_pcm_open(&m_playbackHandle,outputDevice.id.c_str(), 
					SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
		if(err < 0) return std::unexpected(snd_strerror(err));
	}
	
	if(inputDevice.type == Input)
	{
		err = snd_pcm_open(&m_captureHandle, inputDevice.id.c_str(), 
					SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
		if(err < 0) 
		{
			if(m_playbackHandle)
			{
				snd_pcm_close(m_playbackHandle);
				m_playbackHandle = nullptr;
			}
			return std::unexpected(snd_strerror(err));
		}
	}

	if(m_captureHandle && m_playbackHandle)
	{
		snd_pcm_link(m_captureHandle, m_playbackHandle);
	}
	
	snd_pcm_hw_params_alloca(&m_parameter);
	
	err = snd_pcm_hw_params_any(m_playbackHandle, m_parameter);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_any(m_captureHandle, m_parameter);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	// non-interleaved buffer
	err = snd_pcm_hw_params_set_access(
		m_playbackHandle, m_parameter, SND_PCM_ACCESS_RW_NONINTERLEAVED);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_set_access(
		m_captureHandle, m_parameter, SND_PCM_ACCESS_RW_NONINTERLEAVED);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	// bit-depth : 32 bits float
	err = snd_pcm_hw_params_set_format(
		m_playbackHandle, m_parameter, SND_PCM_FORMAT_FLOAT_LE);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	err = snd_pcm_hw_params_set_format(
		m_captureHandle, m_parameter, SND_PCM_FORMAT_FLOAT_LE);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	return {};
}


std::expected<void, std::string> ALSA::close()
{
	int err = 0;
	
	if(m_playbackHandle) {	
		err = snd_pcm_drain(m_playbackHandle);
		if(err < 0) return std::unexpected(snd_strerror(err));

		err = snd_pcm_close(m_playbackHandle);
		if(err < 0) return std::unexpected(snd_strerror(err));
		m_playbackHandle = nullptr;
	}
	
	if(m_captureHandle) {
		err = snd_pcm_drain(m_captureHandle);
		if(err < 0) return std::unexpected(snd_strerror(err));

		err = snd_pcm_close(m_captureHandle);
		if(err < 0) return std::unexpected(snd_strerror(err));
		m_captureHandle = nullptr;
	}

	return {};
}


std::expected<void, std::string> ALSA::setChannels(const unsigned int channel)
{
	AbstractCoreAudio::setChannels(channel);
	return update_alsa_parameter();
}

std::expected<void, std::string> ALSA::setFrames(const unsigned int frames)
{
	AbstractCoreAudio::setFrames(frames);
	return update_alsa_parameter();
}

std::expected<void, std::string> ALSA::setSamplerate(const unsigned int samplerate)
{
	AbstractCoreAudio::setSamplerate(samplerate);
	return update_alsa_parameter();
}

std::expected<void, std::string> ALSA::update_alsa_parameter()
{
	int err = 0;

	// channels
	err = snd_pcm_hw_params_set_channels(
	m_playbackHandle, m_parameter, m_channelCount);
	if(err < 0) return std::unexpected(snd_strerror(err));


	err = snd_pcm_hw_params_set_channels(
	m_captureHandle, m_parameter, m_channelCount);
	if(err < 0) return std::unexpected(snd_strerror(err));
	
	// samplerate
    	err = snd_pcm_hw_params_set_rate_near(
	m_playbackHandle, m_parameter, &m_samplerate, nullptr);
    	if(err < 0) return std::unexpected(snd_strerror(err));

    	err = snd_pcm_hw_params_set_rate_near(
	m_captureHandle, m_parameter, &m_samplerate, nullptr);
    	if(err < 0) return std::unexpected(snd_strerror(err));

    // Taille du buffer et période
	snd_pcm_uframes_t bufferSize = m_frameCount * 4;
    	snd_pcm_uframes_t frameCount = m_frameCount;
	
	err = snd_pcm_hw_params_set_buffer_size_near(
	m_playbackHandle, m_parameter, &bufferSize);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_set_buffer_size_near(
	m_captureHandle, m_parameter, &bufferSize);
    	if(err < 0) return std::unexpected(snd_strerror(err));
    
	err = snd_pcm_hw_params_set_period_size_near(
	m_playbackHandle, m_parameter, &frameCount, nullptr);
	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_set_period_size_near(
	m_captureHandle, m_parameter, &frameCount, nullptr);
	if(err < 0) return std::unexpected(snd_strerror(err));
    
	// apply parameter
    	err = snd_pcm_hw_params(m_playbackHandle, m_parameter);
    	if(err < 0) return std::unexpected(snd_strerror(err));

    	err = snd_pcm_hw_params(m_captureHandle, m_parameter);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	// prepare the devices
    	err = snd_pcm_prepare(m_playbackHandle);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
    	err = snd_pcm_prepare(m_captureHandle);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	return {};
}

