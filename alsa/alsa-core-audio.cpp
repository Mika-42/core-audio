#include "alsa-core-audio.hpp"
#include <algorithm>
#include <cmath>
#include <poll.h>

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
	
	snd_pcm_hw_params_malloc(&m_parameter);
	snd_pcm_hw_params_malloc(&m_parameterCapture);
	
	err = snd_pcm_hw_params_any(m_playbackHandle, m_parameter);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_any(m_captureHandle, m_parameterCapture);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	// non-interleaved buffer
	err = snd_pcm_hw_params_set_access(
		m_playbackHandle, m_parameter, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_set_access(
		m_captureHandle, m_parameterCapture, SND_PCM_ACCESS_MMAP_NONINTERLEAVED);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	// bit-depth : 32 bits float
	err = snd_pcm_hw_params_set_format(
		m_playbackHandle, m_parameter, SND_PCM_FORMAT_FLOAT_LE);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	err = snd_pcm_hw_params_set_format(
		m_captureHandle, m_parameterCapture, SND_PCM_FORMAT_FLOAT_LE);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	return {};
}


std::expected<void, std::string> ALSA::close()
{
	int err = 0;
	snd_pcm_hw_params_free(m_parameter);
	snd_pcm_hw_params_free(m_parameterCapture);

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
	if(channel == 0) return std::unexpected("channel cannot be 0.");
	AbstractCoreAudio::setChannels(channel);
	return update_alsa_parameter();
}

std::expected<void, std::string> ALSA::setFrames(const unsigned int frames)
{
	if(frames == 0) return std::unexpected("frame cannot be 0.");
	AbstractCoreAudio::setFrames(frames);
	return update_alsa_parameter();
}

std::expected<void, std::string> ALSA::setSamplerate(const unsigned int samplerate)
{
	if(samplerate == 0) return std::unexpected("samplerate cannot be 0.");
	AbstractCoreAudio::setSamplerate(samplerate);
	return update_alsa_parameter();
}
 
std::expected<void, std::string> ALSA::audioLoop()
{
    if (!m_playbackHandle) return std::unexpected("playback handle is null.");
	int err = 0;

	float c[2] = {0.0};
	float cc = 2.0f * 3.1415f * 440.0f / (float)m_samplerate;

	unsigned int count = snd_pcm_poll_descriptors_count(m_playbackHandle);

	if(count <= 0)
	{
		return std::unexpected("handle invalid poll descriptor count.");
	}

	std::vector<pollfd> pfds(count);
	err = snd_pcm_poll_descriptors(m_playbackHandle, pfds.data(), count);
	if(err < 0) return std::unexpected(snd_strerror(err));
	
	err = snd_pcm_start(m_playbackHandle);
       	if(err < 0) return std::unexpected(snd_strerror(err));

	for (;;)
    {
	int pollret = poll(pfds.data(), count, 100);

	if (pollret < 0) 
	{
		if(errno == EINTR) continue;

		return std::unexpected(std::string("poll failed: ") + std::strerror(errno))
	} 

	if(pollret == 0) continue;

	unsigned short revents = 0;

	err = snd_pcm_poll_descriptors_revents(m_playbackHandle, pfds.data(), count, &revents);
	if(err < 0) return std::unexpected(snd_strerror(err));

	if(revents & POLLERR)
	{
		snd_pcm_state_t state = snd_pcm_state(m_playbackHandle);
		if (state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_SUSPENDED) 
		{
			err = snd_pcm_recover(m_playbackHandle, -EPIPE, 1);
			if(err < 0) return std::unexpected(snd_strerror(err));

			continue;
		}
	}

	if(!(revents & POLLOUT)) continue;

	snd_pcm_sframes_t avail = snd_pcm_avail_update(m_playbackHandle);
        if (avail < 0) {
            snd_pcm_recover(m_playbackHandle, avail, 1);
            continue;
        }

        if (avail == 0) continue;

	snd_pcm_uframes_t frames = std::min<snd_pcm_uframes_t>(avail, m_frameCount);

        // MMAP direct
        const snd_pcm_channel_area_t* areas;
        snd_pcm_uframes_t offset, nframes = frames;

        int err = snd_pcm_mmap_begin(m_playbackHandle, &areas, &offset, &nframes);
        if (err < 0) {
            snd_pcm_recover(m_playbackHandle, err, 1);
            continue;
        }


        // Écrire sur tous les canaux
        for (unsigned int ch = 0; ch < m_channelCount; ++ch)
        {
		auto base = static_cast<std::byte*>(areas[ch].addr);
		auto byteOffset = (areas[ch].first + areas[ch].step * offset) / 8;
		float* outbuf = reinterpret_cast<float*>(base + byteOffset);

            for(unsigned int i = 0; i < nframes; ++i)
		{
			outbuf[i] = std::sin(c[ch] += cc);
			if(c[ch] > 2.0f * 3.1415f) c[ch] -= 2.0f * 3.1415f;
        	}
	}
	
	err = snd_pcm_mmap_commit(m_playbackHandle, offset, nframes);
        if (err < 0)
	{
            snd_pcm_recover(m_playbackHandle, err, 1);
            continue;
        }
    }
	
	return {};
}

std::expected<void, std::string> ALSA::update_alsa_parameter()
{
	int err = 0;
	
	if(!m_playbackHandle || !m_captureHandle)
        {
		return std::unexpected("ALSA handles is not open");
        }

	// channels
	err = snd_pcm_hw_params_set_channels(
	m_playbackHandle, m_parameter, m_channelCount);
	if(err < 0) return std::unexpected(snd_strerror(err));


	err = snd_pcm_hw_params_set_channels(
	m_captureHandle, m_parameterCapture, m_channelCount);
	if(err < 0) return std::unexpected(snd_strerror(err));
	
	// samplerate
    	err = snd_pcm_hw_params_set_rate_near(
	m_playbackHandle, m_parameter, &m_samplerate, nullptr);
    	if(err < 0) return std::unexpected(snd_strerror(err));

    	err = snd_pcm_hw_params_set_rate_near(
	m_captureHandle, m_parameterCapture, &m_samplerate, nullptr);
    	if(err < 0) return std::unexpected(snd_strerror(err));

    // Taille du buffer et période
	snd_pcm_uframes_t bufferSize = m_frameCount * 4;
    	snd_pcm_uframes_t frameCount = m_frameCount;
	
	err = snd_pcm_hw_params_set_buffer_size_near(
	m_playbackHandle, m_parameter, &bufferSize);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_set_buffer_size_near(
	m_captureHandle, m_parameterCapture, &bufferSize);
    	if(err < 0) return std::unexpected(snd_strerror(err));
    
	err = snd_pcm_hw_params_set_period_size_near(
	m_playbackHandle, m_parameter, &frameCount, nullptr);
	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_hw_params_set_period_size_near(
	m_captureHandle, m_parameterCapture, &frameCount, nullptr);
	if(err < 0) return std::unexpected(snd_strerror(err));
    
	// apply parameter
    	err = snd_pcm_hw_params(m_playbackHandle, m_parameter);
    	if(err < 0) return std::unexpected(snd_strerror(err));

    	err = snd_pcm_hw_params(m_captureHandle, m_parameterCapture);
    	if(err < 0) return std::unexpected(snd_strerror(err));

	// prepare the devices
    	err = snd_pcm_prepare(m_playbackHandle);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
    	err = snd_pcm_prepare(m_captureHandle);
    	if(err < 0) return std::unexpected(snd_strerror(err));
	
	err = snd_pcm_start(m_playbackHandle);
	if(err < 0) return std::unexpected(snd_strerror(err));

	err = snd_pcm_start(m_captureHandle);
        if(err < 0) return std::unexpected(snd_strerror(err));
	return {};
}
