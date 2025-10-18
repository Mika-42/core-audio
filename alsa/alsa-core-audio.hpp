#ifndef ALSA_CORE_AUDIO_HPP
#define ALSA_CORE_AUDIO_HPP

#include "abstract-core-audio.hpp"

#include <alsa/asoundlib.h>

class ALSA final: AbstractCoreAudio 
{
public:
	ALSA() = default;
	
	virtual std::expected<void, std::string> 
	open(const Device& inputDevice, const Device& outputDevice) override;
	
	virtual std::expected<void, std::string> close() override;
 
	virtual std::expected<void, std::string> 
	setChannels(const unsigned int channel) override;
	 
	virtual std::expected<void, std::string> 
	setFrames(const unsigned int frames) override;
	
	virtual std::expected<void, std::string> 
	setSamplerate(const unsigned int samplerate) override;

	std::expected<void, std::string> audioLoop() override;
private:
	snd_pcm_t *m_playbackHandle = nullptr;
	snd_pcm_t *m_captureHandle = nullptr;
	snd_pcm_hw_params_t *m_parameter = nullptr;
	snd_pcm_hw_params_t *m_parameterCapture = nullptr;

	std::expected<void, std::string> update_alsa_parameter();

};
#endif //ALSA_CORE_AUDIO_HPP
