export module audio.alsa;
import audio.abstract_core;

export namespace mka::audio {
/*
	using DeviceID = std::string;

	enum class SampleFormat { Int16, Int24, Int32, Float32, Float64 };

	// What device can do
	struct Capabilities {
		std::vector<size_t> sampleRates;
		std::vector<size_t> bufferSizes;
		std::vector<SampleFormat> sampleFormats;
		size_t inputChannels;
		size_t outputChannels;
	};

	// What I want
	struct DeviceConfig {
		DeviceID deviceID;
		size_t sampleRate;
		size_t bufferSize;
		size_t inputChannels;
		size_t outputChannels;
		SampleFormat sampleFormat;
	};

	enum class State : uint8_t { Closed, Open, Running };
	*/
	class ALSA {
		public:

			virtual std::vector<DeviceID> getDevices() {

			}

			virtual Capabilities getCapabilities(const DeviceID& id) {

			}
		
			// open device with a configuration, in case of fail, do not negotiate. only fail
			virtual bool open(const DeviceConfig& cfg) {

			};

			virtual bool close() {

			}
			
			virtual bool start() {

			}

			virtual bool stop() {

			}
	};
}
