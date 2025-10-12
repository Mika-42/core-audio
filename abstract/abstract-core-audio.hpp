/*
 * This file provide an anstract  class to: 
 *	- access to audio IO
 *	- select number of channels 
 *	- select number of frame
 *	- select a samplerate
 *	- select an available device
 */

#ifndef ABSTRACT_CORE_AUDIO_CPP
#define ABSTRACT_CORE_AUDIO_CPP

#include <expected>
#include <string>
#include <vector>
#include <atomic>
#include <memory>

enum AudioStreamType : bool { Input, Output };

struct Device {
	std::string name;
	std::string id;
	AudioStreamType type;
};

class AbstractCoreAudio
{

public:
	virtual std::expected<void, std::string> 
	open(const Device& inputDevice, const Device& outputDevice) = 0;
	
	virtual	std::expected<void, std::string> close() = 0;

	virtual std::expected<void, std::string> 
	setChannels(const unsigned int channel) { m_channelCount = channel; return {}; }

	virtual std::expected<void, std::string> 
	setFrames(const unsigned int frames) { m_frameCount = frames; return {}; }

	virtual std::expected<void, std::string> 
	setSamplerate(const unsigned int samplerate) 
	{ m_samplerate = samplerate; return {}; }

	virtual const unsigned int& getChannels() const final { return m_channelCount; }
	
	virtual const unsigned int& getFrames() const final { return m_frameCount; }
	
	virtual const unsigned int& getSamplerate() const final { return m_samplerate; }

	virtual void audioLoop() = 0;
	
	virtual ~AbstractCoreAudio(){}

protected:
	unsigned int			m_channelCount;
	unsigned int			m_frameCount;
	unsigned int			m_samplerate;
	
	std::atomic<float**> m_inputBuffers;
	std::atomic<float**> m_outputBuffers;
};

#endif //ABSTRACT_CORE_AUDIO_CPP
