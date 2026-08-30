module;
#include <cstddef>
#include <string>
#include <vector>
export module audio.abstract_core;
export import audio.engine;

/**
 * Backend backend;
 *
 * auto list = backend.getDevices();
 * 
 * auto deviceSelected = list[n];
 *
 * auto capability = backend.getCapabilities(deviceSelected);
 *
 * DeviceConfig config = {
 *   deviceSelected
 *   capability.samplerate[0],
 *   ...
 * };
 *
 * if(backend.open(config).failed()) {
 *		std::println("[param] : x unsupported with [param] y");
 *		std::println("supported values : [a, b, ..., c]");
 *		return;
 * }
 *
 * backend.start();
 *
 * while(isAlive) {
 *	...
 * }
 *
 * backend.stop();
 * backend.close();
 * */
export namespace mka::audio {

	enum class SampleFormat { Int16, Int24, Int32, Float32, Float64, Invalid };
		
	// What I have
	struct DeviceDescriptor {
		std::string id;
		std::string name;
		// capabilities
		std::vector<size_t> sampleRates;
		std::vector<size_t> bufferSizes;
		std::vector<SampleFormat> sampleFormats;
		size_t inputChannels = 0;
		size_t outputChannels = 0;
	};
	
	// What I want
	struct DeviceConfig {
		std::string id;

		size_t sampleRate = 0;
		size_t bufferSize = 0;
		SampleFormat sampleFormat;
		size_t inputChannels = 0;
		size_t outputChannels = 0;
	};

	enum class State { Closed, Open, Running };
	
	class Backend {
		public:
			Backend() = default;
			Backend(const Backend&) = delete;
			Backend& operator=(const Backend&) = delete;
			Backend(Backend&&) = delete;
			Backend& operator=(Backend&&) = delete;

			virtual ~Backend() = default;

			virtual std::vector<DeviceDescriptor> getDevices() = 0;
	
			virtual void setCallback(ProcessBlockFn processBlock) final {
				processBlock_ = processBlock;	
			}
				
			// open device with a configuration, in case of fail, do not negotiate. only fail
			virtual bool open(const DeviceConfig& cfg) = 0;
			virtual bool close() = 0;
			
			virtual bool start() = 0;
			virtual bool stop() = 0;
			
			static constexpr size_t SUPPORTED_SAMPLE_RATES[] = {8000,11025,16000,22050,32000,44100,48000,88200,96000, 176400,192000,352800,384000 };
			static constexpr size_t SUPPORTED_BUFFER_SIZES[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
		
		protected:
			ProcessBlockFn processBlock_ = nullptr;

	};
}
