module;
#include <jack/jack.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <print>

export module audio.jack;
import audio.abstract_core;
import audio.error;
import audio.config;
import audio.constants;

namespace mka::audio {

	int processCallback(jack_nframes_t nframes, void* arg);
	int sampleRateCallback(jack_nframes_t nframes, void* arg);
	int bufferSizeCallback(jack_nframes_t nframes, void* arg);
	int xrunCallback(void* arg);
	void shutdownCallback(void* arg);

	export class JACK final : public Device {
	public:
		JACK() = default;
		~JACK() override { closeNoLock(); }

		static void enumerateDevices() {
			jack_client_t *client = jack_client_open("DeviceEnumerator", JackNoStartServer, NULL);
			if (client == NULL) {
				std::println("Impossible de se connecter à JACK");
				return;
			}
        
			// Obtenir les ports audio
			const char **ports = jack_get_ports(client, nullptr, nullptr, 0);
			if (ports) {
				for (int i = 0; ports[i]; i++) {
					std::println("{}",ports[i]);
				}
				jack_free(ports);
			}
			
			jack_client_close(client);
		}

		[[nodiscard]] Result open(const DeviceConfig& cfg) override {
			std::lock_guard lock(controlMutex_);

			if (state_.load(std::memory_order_acquire) != State::Closed) {
				return Result{ Error::WouldBlock, "Device must be closed before opening." };
			}

			jack_status_t status{};
			client_ = jack_client_open(
				cfg.deviceID.empty() ? "mka_audio" : cfg.deviceID.c_str(),
				JackNoStartServer,
				&status
			);
			if (!client_) {
				return Result{ Error::DeviceOpenFailed, "Failed to open JACK client." };
			}

			jack_on_shutdown(client_, shutdownCallback, this);
			jack_set_xrun_callback(client_, xrunCallback, this);
			jack_set_process_callback(client_, processCallback, this);
			jack_set_sample_rate_callback(client_, sampleRateCallback, this);
			jack_set_buffer_size_callback(client_, bufferSizeCallback, this);

			if (!registerPorts(cfg)) {
				jack_client_close(client_);
				client_ = nullptr;
				return Result{ Error::DeviceOpenFailed, "Failed to register JACK ports." };
			}

			// JACK impose son propre sample rate / buffer size côté serveur ;
			// open() ne peut que les LIRE après coup, jamais les forcer via cfg.
			// C'est la négociation documentée dans le contrat de Device::open().
			info_.id              = cfg.deviceID;
			info_.name             = "JACK";
			info_.sampleRate       = jack_get_sample_rate(client_);
			info_.bufferSize       = jack_get_buffer_size(client_);
			info_.inputChannels    = static_cast<uint32_t>(inputPorts_.size());
			info_.outputChannels   = static_cast<uint32_t>(outputPorts_.size());
			info_.inputLatencyMs   = 0.0;
			info_.outputLatencyMs  = 0.0;
			// JACK_DEFAULT_AUDIO_TYPE est toujours du float : cfg.preferredFormat
			// est ignoré ici, JACK ne négocie pas le format d'échantillon.
			info_.sampleFormat     = SampleFormat::Float32;

			inputScratch_.assign(inputPorts_.size(), nullptr);
			outputScratch_.assign(outputPorts_.size(), nullptr);

			state_.store(State::Open, std::memory_order_release);
			return mka::audio::Ok;
		}

		[[nodiscard]] Result close() override {
			std::lock_guard lock(controlMutex_);
			return closeNoLock();
		}

		[[nodiscard]] Result start() override {
			std::lock_guard lock(controlMutex_);
			if (state_.load(std::memory_order_acquire) != State::Open) {
				return Result{ Error::WouldBlock, "Device must be open before starting." };
			}
			if (jack_activate(client_) != 0) {
				return Result{ Error::GenericError, "jack_activate failed." };
			}
			state_.store(State::Running, std::memory_order_release);
			return mka::audio::Ok;
		}

		[[nodiscard]] Result stop() override {
			std::lock_guard lock(controlMutex_);
			return stopNoLock();
		}

	private:
		friend int processCallback(jack_nframes_t, void*);
		friend int sampleRateCallback(jack_nframes_t, void*);
		friend int bufferSizeCallback(jack_nframes_t, void*);
		friend int xrunCallback(void*);
		friend void shutdownCallback(void*);

		bool registerPorts(const DeviceConfig& cfg) {
			inputPorts_.reserve(cfg.inputChannels);
			for (uint32_t i = 0; i < cfg.inputChannels; ++i) {
				std::string name = "input_" + std::to_string(i);
				jack_port_t* port = jack_port_register(
					client_, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
				if (!port) return false;
				inputPorts_.push_back(port);
			}
			outputPorts_.reserve(cfg.outputChannels);
			for (uint32_t i = 0; i < cfg.outputChannels; ++i) {
				std::string name = "output_" + std::to_string(i);
				jack_port_t* port = jack_port_register(
					client_, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
				if (!port) return false;
				outputPorts_.push_back(port);
			}
			return true;
		}

		Result stopNoLock() {
			if (state_.load(std::memory_order_acquire) != State::Running) {
				return mka::audio::Ok; // idempotent
			}
			jack_deactivate(client_);
			state_.store(State::Open, std::memory_order_release);
			return mka::audio::Ok;
		}

		Result closeNoLock() {
			if (state_.load(std::memory_order_acquire) == State::Closed) {
				return mka::audio::Ok; // idempotent
			}
			stopNoLock();

			if (client_) {
				for (auto* p : inputPorts_)  if (p) jack_port_unregister(client_, p);
				for (auto* p : outputPorts_) if (p) jack_port_unregister(client_, p);
				jack_client_close(client_);
				client_ = nullptr;
			}
			inputPorts_.clear();
			outputPorts_.clear();
			inputScratch_.clear();
			outputScratch_.clear();

			state_.store(State::Closed, std::memory_order_release);
			return mka::audio::Ok;
		}

		jack_client_t* client_ = nullptr;
		std::vector<jack_port_t*> inputPorts_;
		std::vector<jack_port_t*> outputPorts_;

		// Réutilisés à chaque callback : aucune allocation dans le thread audio.
		std::vector<float*> inputScratch_;
		std::vector<float*> outputScratch_;

		std::atomic<size_t> xrunCount_{0};

		// Protège open/close/start/stop uniquement. Jamais touché par processCallback.
		std::mutex controlMutex_;
	};

	// ---- Callbacks JACK (thread(s) internes à JACK, pas le thread de contrôle) ----

	int processCallback(jack_nframes_t nframes, void* arg) {
		auto* dev = static_cast<JACK*>(arg);
		if (!dev->callback_) return 0;

		for (size_t i = 0; i < dev->inputPorts_.size(); ++i) {
			dev->inputScratch_[i] = static_cast<float*>(
				jack_port_get_buffer(dev->inputPorts_[i], nframes));
		}
		for (size_t i = 0; i < dev->outputPorts_.size(); ++i) {
			dev->outputScratch_[i] = static_cast<float*>(
				jack_port_get_buffer(dev->outputPorts_[i], nframes));
		}

		Buffer buffer{
			.inputs      = dev->inputScratch_.data(),
			.outputs     = dev->outputScratch_.data(),
			.inputCount  = static_cast<uint32_t>(dev->inputPorts_.size()),
			.outputCount = static_cast<uint32_t>(dev->outputPorts_.size()),
			.frames      = static_cast<uint32_t>(nframes),
		};

		dev->callback_(buffer, dev->userData_);
		return 0;
	}

	int sampleRateCallback(jack_nframes_t nframes, void* arg) {
		auto* dev = static_cast<JACK*>(arg);
		// Rare (changement serveur), pas de lock ici pour rester realtime-safe :
		// on tolère une incohérence transitoire de info_.sampleRate.
		dev->info_.sampleRate = nframes;
		return 0;
	}

	int bufferSizeCallback(jack_nframes_t nframes, void* arg) {
		auto* dev = static_cast<JACK*>(arg);
		dev->info_.bufferSize = nframes;
		return 0;
	}

	int xrunCallback(void* arg) {
		auto* dev = static_cast<JACK*>(arg);
		dev->xrunCount_.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}

	void shutdownCallback(void* arg) {
		auto* dev = static_cast<JACK*>(arg);
		// JACK a déjà libéré le client à ce stade : ne surtout pas rappeler
		// jack_client_close dessus. On marque juste l'état.
		dev->client_ = nullptr;
		dev->state_.store(State::Closed, std::memory_order_release);
	}

} // namespace mka::audio
