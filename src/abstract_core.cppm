module;

#include <memory>
#include <string>
#include <atomic>
#include <vector>

export module audio.abstract_core;
import audio.error;
import audio.block;
import audio.config;

export namespace mka::audio {

	enum class State : uint8_t { Stopped, Starting, Running, Stopping, Closed };

	/*
	 * ! callback must be set before start() !
	 */
	class AbstractCoreAudio
	{

	public:
		virtual ~AbstractCoreAudio() {}

		virtual std::vector<Channel> getChannels() = 0;
	
		virtual Result open(const Channel& channel) = 0;	
		virtual	Result close() = 0;

		virtual void start() = 0;

		virtual void stop() = 0;	

		virtual void setCallback(const Callback& callback) final {
			this->callback = callback;	
		}

		virtual void setSampleRate(uint32_t sampleRate) final {
			this->sampleRate.store(sampleRate);
		}

		virtual uint32_t getSampleRate() final {
			return sampleRate.load();
		}

		virtual void setBufferSize(uint32_t bufferSize) final {
			this->bufferSize.store(bufferSize);
		}

		virtual uint32_t getBufferSize() final {
			return bufferSize.load();
		}

	protected:
		virtual void run() = 0;

		Callback				callback = nullptr;

		std::atomic<State>		state		= State::Closed;
		std::atomic<uint32_t>	sampleRate	= 0;
		std::atomic<uint32_t>	bufferSize	= 0;
	
	};
}

