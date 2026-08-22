module;
#include <cstdint>
#include <string>
#include <atomic>
export module audio.abstract_core;
import audio.error;

export namespace mka::audio {

	enum class State : uint8_t { Closed, Open, Running };

	enum class SampleFormat { Int16, Int24, Int32, Float32, Float64 };

	struct DeviceConfig {
		std::string deviceID;
		uint32_t sampleRate       = 44100;
		uint32_t bufferSize       = 256;
		uint32_t inputChannels    = 2;
		uint32_t outputChannels   = 2;
		SampleFormat preferredFormat = SampleFormat::Float32;
	};

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

		//[[nodiscard]] virtual Result routePort(Port io, const std::string& destPortName) = 0;

		[[nodiscard]] virtual Result reopen(const DeviceConfig& cfg) {
			if (state_.load(std::memory_order_acquire) == State::Running) {
				if (auto r = stop(); !r) return r;
			}
			if (state_.load(std::memory_order_acquire) != State::Closed) {
				if (auto r = close(); !r) return r;
			}
			return open(cfg);
		}

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
}
