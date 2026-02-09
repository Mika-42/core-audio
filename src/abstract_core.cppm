module;

#include <memory>
#include <string>
#include <thread>
#include <atomic>

export module audio.abstract_core;
import audio.error;
import audio.block;
import audio.config;

export namespace mka::audio {
	/*
	 * ! callback must be set before start() !
	 */
	class AbstractCoreAudio
	{

	public:
		virtual ~AbstractCoreAudio() {
			stop();
		}
	
		virtual Result open(const Config& config) = 0;	
		virtual	Result close() = 0;

		virtual void start() {
			if(running.exchange(true)) return;
			audioThread = std::thread(&AbstractCoreAudio::run, this);
		}

		virtual void stop() {
			if(!running.exchange(false)) return;
			if(audioThread.joinable()) audioThread.join();
		}	

		virtual void setCallback(const Callback& callback) final {
			this->callback = callback;	
		}

	protected:
		virtual void run() = 0;

		Config				config;
		Callback			callback = nullptr;
		std::atomic<bool>	running	= false;

	private:
		std::thread audioThread;
	};
}

