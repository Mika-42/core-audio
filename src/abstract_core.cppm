module;

#include <mutex>
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

	class AbstractCoreAudio
	{

	public:
		virtual ~AbstractCoreAudio() {}

		virtual std::vector<ChannelInfo> getChannels() = 0;
		virtual std::vector<ChannelInfo> getMidiDevices() = 0;
		virtual Result mapMidiDevice(const char* deviceName, uint8_t channel) = 0;
	
		virtual Result open(const ChannelInfo channel) = 0;	
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

		virtual void setBlockSize(uint32_t blockSize) final {
			this->blockSize.store(blockSize);
		}

		virtual uint32_t getBlockSize() final {
			return blockSize.load();
		}

		virtual bool isRunning() const final {
			return state.load() == State::Running;
		}

		virtual RuntimeStats getRuntimeStats() const {
			return RuntimeStats {};
		}

	protected:
		virtual void run() = 0;

		Callback				callback = nullptr;
		
		std::mutex				lifecycleMutex;
		std::atomic<State>		state		= State::Closed;
		std::atomic<uint32_t>	sampleRate	= 0;
		std::atomic<uint32_t>	blockSize	= 0;
	
	};
}

