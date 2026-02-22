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

	protected:
		virtual void run() = 0;

		Callback			callback = nullptr;
		std::atomic<bool>	running	= false;
	
	};
}

