module;

#include <memory>
#include <string>
#include <thread>
#include <atomic>

#include <pthread.h>
#include <sched.h>

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

		virtual std::vector<Device> devicesList() = 0;
	
		virtual Result open(const Config& config) = 0;	
		virtual	Result close() = 0;

		virtual void start() {
			if(running.exchange(true)) return;
			audioThread = std::thread(&AbstractCoreAudio::run, this);

			// thread priority
			sched_param param = {};
			param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 5;
			pthread_setschedparam(audioThread.native_handle(), SCHED_FIFO, &param);

			cpu_set_t cpuset;
			CPU_ZERO(&cpuset);
			CPU_SET(0, &cpuset);
			pthread_setaffinity_np(audioThread.native_handle(), sizeof(cpu_set_t), &cpuset);

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

