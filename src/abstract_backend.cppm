module;
#include <cstddef>
#include <string>
#include <vector>
export module audio.abstract_core;

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
	
	class Backend {
		public:
			Backend(const Backend&) = delete;
			Backend& operator=(const Backend&) = delete;
			Backend(Backend&&) = delete;
			Backend& operator=(Backend&&) = delete;

			virtual ~Backend() = default;

			virtual std::vector<DeviceID> getDevices() = 0;

			virtual Capabilities getCapabilities(const DeviceID& id) = 0;
		
			// open device with a configuration, in case of fail, do not negotiate. only fail
			virtual bool open(const DeviceConfig& cfg) = 0;
			virtual bool close() = 0;
			
			virtual bool start() = 0;
			virtual bool stop() = 0;
	};
}
/*	//-----
	enum class State : uint8_t { Closed, Open, Running };

	struct DeviceInfo {
		std::string id;
		std::string name;
		uint32_t sampleRate     = 0;
		uint32_t bufferSize     = 0;
		uint32_t inputChannels  = 0;
		uint32_t outputChannels = 0;
		double inputLatencyMs   = 0.0;
		double outputLatencyMs  = 0.0;
		SampleFormat sampleFormat = SampleFormat::Float32;
	};

	// Contrat : pointeurs valides UNIQUEMENT pendant la durée de l'appel callback.
	// Le backend reste propriétaire des buffers ; ne jamais les stocker/réutiliser
	// après le retour du callback. Bloc supposé contigu et fourni tel quel par
	// le backend concret — aucune garantie de continuité temporelle au niveau
	// de cette interface abstraite.
	struct Buffer {
		float* const* inputs;
		float* const* outputs;
		uint32_t inputCount;
		uint32_t outputCount;
		uint32_t frames;
	};

	using Callback = void(*)(Buffer&, void* userData);

	class Device {
	public:
		virtual ~Device() = default;

		Device(const Device&) = delete;
		Device& operator=(const Device&) = delete;
		Device(Device&&) = delete;
		Device& operator=(Device&&) = delete;

		// Control plane : ne jamais appeler open/close/start/stop/reopen
		// depuis le thread du callback audio (realtime plane).
		//
		// open(cfg) est une NÉGOCIATION, pas une garantie : le backend peut
		// ajuster sampleRate/bufferSize/channels selon ses contraintes.
		// Postcondition stricte : si open() retourne un succès, info_ DOIT
		// être entièrement à jour et refléter l'état réel avant tout appel
		// à start(). Un backend qui retourne succès sans info_ à jour
		// viole le contrat de l'interface.
		[[nodiscard]] virtual Result open(const DeviceConfig& cfg) = 0;
		[[nodiscard]] virtual Result close() = 0;
		[[nodiscard]] virtual Result start() = 0;
		[[nodiscard]] virtual Result stop() = 0;

		void setCallback(Callback callback, void* userData = nullptr) {
			callback_ = callback;
			userData_ = userData;
		}

		[[nodiscard]] const DeviceInfo& info() const { return info_; }

		// Safe à appeler depuis n'importe quel thread, y compris le callback.
		[[nodiscard]] State state() const { return state_.load(std::memory_order_acquire); }

	protected:
		Device() = default;

		template<typename T>
		[[nodiscard]] T* userData() const noexcept {
			return static_cast<T*>(userData_);
		}

		Callback callback_ = nullptr;
		void*    userData_ = nullptr;

		std::atomic<State> state_{State::Closed};
		DeviceInfo          info_ = {};
	};
}*/
