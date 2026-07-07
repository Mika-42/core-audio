module;
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sys/mman.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

export module audio.alsa;
import audio.abstract_core;
import audio.error;
import audio.config;

namespace mka::audio {

	namespace {
		// Formats essayés dans l'ordre de préférence : le plus proche de float
		// d'abord, on redescend en précision seulement si le device ne suit pas.
		// Permet à hw: (accès direct, sans conversion driver) de fonctionner
		// même sur du matériel qui ne parle pas Float32 nativement.
		constexpr snd_pcm_format_t kFormatCandidates[] = {
			SND_PCM_FORMAT_FLOAT_LE,
			SND_PCM_FORMAT_S32_LE,
			SND_PCM_FORMAT_S24_3LE,
			SND_PCM_FORMAT_S16_LE
		};

		SampleFormat toSampleFormat(snd_pcm_format_t fmt) {
			switch (fmt) {
				case SND_PCM_FORMAT_FLOAT_LE: return SampleFormat::Float32;
				case SND_PCM_FORMAT_S32_LE:   return SampleFormat::Int32;
				case SND_PCM_FORMAT_S24_3LE:  return SampleFormat::Int24;
				case SND_PCM_FORMAT_S16_LE:   return SampleFormat::Int16;
				default:                      return SampleFormat::Float32;
			}
		}

		size_t bytesPerSample(snd_pcm_format_t fmt) {
			switch (fmt) {
				case SND_PCM_FORMAT_FLOAT_LE: return 4;
				case SND_PCM_FORMAT_S32_LE:   return 4;
				case SND_PCM_FORMAT_S24_3LE:  return 3;
				case SND_PCM_FORMAT_S16_LE:   return 2;
				default:                      return 4;
			}
		}

		float pcmToFloat(const uint8_t* src, snd_pcm_format_t fmt) {
			switch (fmt) {
				case SND_PCM_FORMAT_FLOAT_LE:
					return *reinterpret_cast<const float*>(src);
				case SND_PCM_FORMAT_S32_LE: {
					int32_t v = *reinterpret_cast<const int32_t*>(src);
					return static_cast<float>(v) / 2147483648.0f;
				}
				case SND_PCM_FORMAT_S24_3LE: {
					int32_t v = src[0] | (src[1] << 8) | (src[2] << 16);
					if (v & 0x800000) v |= static_cast<int32_t>(0xFF000000u);
					return static_cast<float>(v) / 8388608.0f;
				}
				case SND_PCM_FORMAT_S16_LE: {
					int16_t v = *reinterpret_cast<const int16_t*>(src);
					return static_cast<float>(v) / 32768.0f;
				}
				default: return 0.0f;
			}
		}

		void floatToPcm(float sample, uint8_t* dst, snd_pcm_format_t fmt) {
			sample = std::clamp(sample, -1.0f, 1.0f);
			switch (fmt) {
				case SND_PCM_FORMAT_FLOAT_LE:
					*reinterpret_cast<float*>(dst) = sample;
					break;
				case SND_PCM_FORMAT_S32_LE: {
					// 2147483647.0f n'est pas exactement représentable en float32 : le
					// compilateur l'arrondit à 2147483648.0f (2^31), ce qui déborde
					// INT32_MAX pour sample == 1.0 et cause un undefined behavior au
					// cast (wraparound audible en clics/distorsion). On passe par un
					// calcul en double, puis on clamp explicitement avant le cast final.
					double scaled = static_cast<double>(sample) * 2147483647.0;
					int64_t clamped = std::clamp(
						static_cast<int64_t>(scaled),
						static_cast<int64_t>(INT32_MIN),
						static_cast<int64_t>(INT32_MAX)
					);
					*reinterpret_cast<int32_t*>(dst) = static_cast<int32_t>(clamped);
					break;
				}
				case SND_PCM_FORMAT_S24_3LE: {
					int32_t v = static_cast<int32_t>(sample * 8388607.0f);
					dst[0] = static_cast<uint8_t>(v & 0xFF);
					dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
					dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
					break;
				}
				case SND_PCM_FORMAT_S16_LE: {
					int16_t v = static_cast<int16_t>(sample * 32767.0f);
					*reinterpret_cast<int16_t*>(dst) = v;
					break;
				}
				default: break;
			}
		}
	}

	// deviceID accepte la syntaxe standard ALSA :
	//   "hw:2,0"      -> accès direct exclusif, latence minimale, vire tout autre
	//                     consommateur (PipeWire/PulseAudio inclus) de la carte.
	//                     Le format PCM natif est négocié automatiquement
	//                     (Float32 -> Int32 -> Int24 -> Int16), donc fonctionne
	//                     même sur du matériel qui ne parle pas float nativement.
	//   "plughw:2,0"  -> accès converti/partagé, coexiste avec le reste du système
	//                     (PipeWire, dmix), conversion de format/rate automatique
	//                     déjà assurée par le plugin ALSA.
	//   "default"     -> device par défaut du système, toujours partagé.
	export class ALSA final : public Device {
	public:
		ALSA() = default;
		~ALSA() override { closeNoLock(); }

		struct DeviceDescriptor {
			std::string hardwareID; // ex: "hw:2,0"
			std::string name;       // ex: "Focusrite Scarlett 2i2 USB"

			[[nodiscard]] std::string sharedID() const { return "plug" + hardwareID; }
		};

		// Thread-safe : aucun état partagé muté, retour par valeur.
		[[nodiscard]] std::vector<DeviceDescriptor> enumerateDevices() const {
			std::vector<DeviceDescriptor> devices;

			int card = -1;
			while (snd_card_next(&card) >= 0 && card >= 0) {
				const std::string ctlName = "hw:" + std::to_string(card);

				snd_ctl_t* ctl = nullptr;
				if (snd_ctl_open(&ctl, ctlName.c_str(), 0) < 0) continue;

				snd_ctl_card_info_t* cardInfo = nullptr;
				snd_ctl_card_info_alloca(&cardInfo);
				if (snd_ctl_card_info(ctl, cardInfo) < 0) {
					snd_ctl_close(ctl);
					continue;
				}
				const std::string cardName = snd_ctl_card_info_get_name(cardInfo);

				int device = -1;
				while (snd_ctl_pcm_next_device(ctl, &device) >= 0 && device >= 0) {
					devices.push_back({
						.hardwareID = "hw:" + std::to_string(card) + "," + std::to_string(device),
						.name = cardName
					});
				}
				snd_ctl_close(ctl);
			}
			return devices;
		}

		[[nodiscard]] Result open(const DeviceConfig& cfg) override {
			std::lock_guard lock(controlMutex_);
			return openNoLock(cfg);
		}

		[[nodiscard]] Result close() override {
			std::lock_guard lock(controlMutex_);
			return closeNoLock();
		}

		[[nodiscard]] Result start() override {
			std::lock_guard lock(controlMutex_);
			return startNoLock();
		}

		[[nodiscard]] Result stop() override {
			std::lock_guard lock(controlMutex_);
			return stopNoLock();
		}

		// Atomique bout-en-bout : un seul verrou tenu sur toute la séquence
		// stop -> close -> open, aucune fenêtre où un autre thread de contrôle
		// pourrait s'insérer entre deux étapes.
		[[nodiscard]] Result reopen(const DeviceConfig& cfg) override {
			std::lock_guard lock(controlMutex_);
			if (state_.load(std::memory_order_acquire) == State::Running) {
				if (auto r = stopNoLock(); !r) return r;
			}
			if (state_.load(std::memory_order_acquire) != State::Closed) {
				if (auto r = closeNoLock(); !r) return r;
			}
			return openNoLock(cfg);
		}

		// Télémétrie temps réel safe : détail input/output plutôt qu'un
		// compteur agrégé qui masquerait l'origine du glitch.
		[[nodiscard]] size_t xrunCount() const noexcept {
			return inputXrunCount_.load(std::memory_order_relaxed)
			     + outputXrunCount_.load(std::memory_order_relaxed);
		}
		[[nodiscard]] size_t inputXrunCount() const noexcept {
			return inputXrunCount_.load(std::memory_order_relaxed);
		}
		[[nodiscard]] size_t outputXrunCount() const noexcept {
			return outputXrunCount_.load(std::memory_order_relaxed);
		}
		[[nodiscard]] size_t suspendRecoveryCount() const noexcept {
			return suspendRecoveryCount_.load(std::memory_order_relaxed);
		}

		[[nodiscard]] Result lastError() const {
			std::lock_guard lock(lastErrorMutex_);
			return lastError_;
		}

		// Diagnostic honnête : l'appelant peut savoir si la priorité temps
		// réel a réellement été obtenue par le thread audio, pour avertir
		// l'utilisateur si ce n'est pas le cas (permissions systeme).
		[[nodiscard]] bool isRealtimeThread() const noexcept {
			return realtimeThreadActive_.load(std::memory_order_relaxed);
		}
		[[nodiscard]] bool isMemoryLocked() const noexcept {
			return memoryLocked_.load(std::memory_order_relaxed);
		}

	private:
		void setLastError(Result r) {
			std::lock_guard lock(lastErrorMutex_);
			lastError_ = std::move(r);
		}
		void clearLastError() {
			std::lock_guard lock(lastErrorMutex_);
			lastError_ = mka::audio::Ok;
		}

		[[nodiscard]] Result openNoLock(const DeviceConfig& cfg) {
			if (state_.load(std::memory_order_acquire) != State::Closed) {
				return Result{ Error::WouldBlock, "Device must be closed before opening." };
			}
			if (cfg.inputChannels == 0 && cfg.outputChannels == 0) {
				return Result{ Error::GenericError, "No input or output channels requested." };
			}
			if (cfg.sampleRate == 0) {
				return Result{ Error::GenericError, "sampleRate cannot be 0." };
			}
			if (cfg.bufferSize == 0) {
				return Result{ Error::GenericError, "bufferSize cannot be 0." };
			}

			uint32_t captureRate = 0, capturePeriod = 0;
			uint32_t playbackRate = 0, playbackPeriod = 0;
			snd_pcm_format_t captureFmt  = SND_PCM_FORMAT_FLOAT_LE;
			snd_pcm_format_t playbackFmt = SND_PCM_FORMAT_FLOAT_LE;

			if (cfg.inputChannels > 0) {
				if (auto r = openStream(captureHandle_, cfg, SND_PCM_STREAM_CAPTURE,
				                          cfg.inputChannels, captureRate, capturePeriod, captureFmt); !r) {
					closeNoLock();
					return r;
				}
			}
			if (cfg.outputChannels > 0) {
				if (auto r = openStream(playbackHandle_, cfg, SND_PCM_STREAM_PLAYBACK,
				                          cfg.outputChannels, playbackRate, playbackPeriod, playbackFmt); !r) {
					closeNoLock();
					return r;
				}
			}

			// Si les deux flux sont ouverts, ils DOIVENT converger vers le même
			// rate/period. Sinon on refuse plutôt que d'écraser silencieusement.
			// Les formats natifs, eux, peuvent légitimement différer entre
			// capture et playback (deux convertisseurs physiques distincts).
			uint32_t negotiatedRate, negotiatedPeriod;
			if (captureHandle_ && playbackHandle_) {
				if (captureRate != playbackRate || capturePeriod != playbackPeriod) {
					closeNoLock();
					return Result{ Error::DeviceOpenFailed,
						"Capture and playback streams negotiated different rate/period; "
						"this device configuration is not supported." };
				}
				negotiatedRate = captureRate;
				negotiatedPeriod = capturePeriod;
			} else if (captureHandle_) {
				negotiatedRate = captureRate;
				negotiatedPeriod = capturePeriod;
			} else {
				negotiatedRate = playbackRate;
				negotiatedPeriod = playbackPeriod;
			}

			alsaCaptureFormat_  = captureFmt;
			alsaPlaybackFormat_ = playbackFmt;

			info_.id              = cfg.deviceID;
			info_.name             = cfg.deviceID;
			info_.sampleRate       = negotiatedRate;
			info_.bufferSize       = negotiatedPeriod;
			info_.inputChannels    = cfg.inputChannels;
			info_.outputChannels   = cfg.outputChannels;
			// Priorité au format de sortie (contrainte la plus souvent pertinente
			// pour l'appelant), sinon celui de la capture si pas de sortie.
			info_.sampleFormat = cfg.outputChannels > 0
				? toSampleFormat(playbackFmt) : toSampleFormat(captureFmt);

			try {
				allocateBuffers(cfg.inputChannels, cfg.outputChannels, negotiatedPeriod);
			} catch (const std::exception&) {
				closeNoLock();
				return Result{ Error::GenericError, "Failed to allocate audio buffers (out of memory)." };
			}

			clearLastError();
			inputXrunCount_.store(0, std::memory_order_relaxed);
			outputXrunCount_.store(0, std::memory_order_relaxed);
			suspendRecoveryCount_.store(0, std::memory_order_relaxed);

			state_.store(State::Open, std::memory_order_release);
			return mka::audio::Ok;
		}

	[[nodiscard]] Result startNoLock() {
		if (state_.load(std::memory_order_acquire) != State::Open) {
			return Result{ Error::WouldBlock, "Device must be open before starting." };
		}
		if (captureHandle_) {
			if (snd_pcm_prepare(captureHandle_) < 0) {
				return Result{ Error::GenericError, "snd_pcm_prepare failed on capture stream." };
			}
		}
		if (playbackHandle_) {
			if (snd_pcm_prepare(playbackHandle_) < 0) {
				return Result{ Error::GenericError, "snd_pcm_prepare failed on playback stream." };
			}
		}

		// Latence mesurable seulement une fois le flux préparé/en cours.
		info_.inputLatencyMs  = queryLatencyMs(captureHandle_, info_.sampleRate);
		info_.outputLatencyMs = queryLatencyMs(playbackHandle_, info_.sampleRate);

		clearLastError();
		running_.store(true, std::memory_order_release);
		audioThread_ = std::thread(&ALSA::audioLoop, this);

		state_.store(State::Running, std::memory_order_release);
		return mka::audio::Ok;
	}

		[[nodiscard]] Result openStream(snd_pcm_t*& handle, const DeviceConfig& cfg,
		                                  snd_pcm_stream_t stream, uint32_t channels,
		                                  uint32_t& outRate, uint32_t& outPeriod,
		                                  snd_pcm_format_t& outFormat) {
			if (snd_pcm_open(&handle, cfg.deviceID.c_str(), stream, 0) < 0) {
				return Result{ Error::DeviceOpenFailed,
					"snd_pcm_open failed. Verifie deviceID (hw:X,Y / plughw:X,Y / default)." };
			}

			snd_pcm_hw_params_t* hw = nullptr;
			snd_pcm_hw_params_alloca(&hw);
			snd_pcm_hw_params_any(handle, hw);
			snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);

			// Négociation de format : on essaie chaque candidat jusqu'à ce que
			// la carte en accepte un. Fonctionne aussi bien en hw: (natif) qu'en
			// plughw: (déjà converti par ALSA de toute facon).
			snd_pcm_format_t chosen = SND_PCM_FORMAT_UNKNOWN;
			for (auto fmt : kFormatCandidates) {
				if (snd_pcm_hw_params_test_format(handle, hw, fmt) == 0) {
					chosen = fmt;
					break;
				}
			}
			if (chosen == SND_PCM_FORMAT_UNKNOWN) {
				snd_pcm_close(handle);
				handle = nullptr;
				return Result{ Error::DeviceOpenFailed, "Aucun format PCM candidat supporte par ce device." };
			}
			snd_pcm_hw_params_set_format(handle, hw, chosen);
			outFormat = chosen;

			if (snd_pcm_hw_params_set_channels(handle, hw, channels) < 0) {
				snd_pcm_close(handle);
				handle = nullptr;
				return Result{ Error::DeviceOpenFailed, "Nombre de canaux non supporte par ce device." };
			}

			uint32_t rate = cfg.sampleRate;
			if (snd_pcm_hw_params_set_rate_near(handle, hw, &rate, nullptr) < 0) {
				snd_pcm_close(handle);
				handle = nullptr;
				return Result{ Error::DeviceOpenFailed, "sampleRate rejected by driver." };
			}

			snd_pcm_uframes_t period = cfg.bufferSize;
			if (snd_pcm_hw_params_set_period_size_near(handle, hw, &period, nullptr) < 0) {
				snd_pcm_close(handle);
				handle = nullptr;
				return Result{ Error::DeviceOpenFailed, "bufferSize rejected by driver." };
			}

			// Borne explicitement le nombre de périodes du ring buffer. Sans ça,
			// certains drivers choisissent 4-8 périodes par défaut, multipliant
			// la latence réelle sans que l'appelant ne le sache.
			unsigned int periods = kPeriodsCount;
			snd_pcm_hw_params_set_periods_near(handle, hw, &periods, nullptr);

			if (snd_pcm_hw_params(handle, hw) < 0) {
				snd_pcm_close(handle);
				handle = nullptr;
				return Result{ Error::DeviceOpenFailed, "snd_pcm_hw_params rejected by driver." };
			}

			outRate   = rate;
			outPeriod = static_cast<uint32_t>(period);
			return mka::audio::Ok;
		}

		static double queryLatencyMs(snd_pcm_t* handle, uint32_t sampleRate) {
			if (!handle || sampleRate == 0) return 0.0;
			snd_pcm_sframes_t delayFrames = 0;
			if (snd_pcm_delay(handle, &delayFrames) < 0) return 0.0;
			return 1000.0 * static_cast<double>(delayFrames) / sampleRate;
		}

		void allocateBuffers(uint32_t inCh, uint32_t outCh, uint32_t frames) {
			inputPlanar_.assign(static_cast<size_t>(inCh) * frames, 0.0f);
			outputPlanar_.assign(static_cast<size_t>(outCh) * frames, 0.0f);
			inputChannelPtrs_.resize(inCh);
			outputChannelPtrs_.resize(outCh);
			for (uint32_t c = 0; c < inCh; ++c)
				inputChannelPtrs_[c] = &inputPlanar_[static_cast<size_t>(c) * frames];
			for (uint32_t c = 0; c < outCh; ++c)
				outputChannelPtrs_[c] = &outputPlanar_[static_cast<size_t>(c) * frames];

			// Buffers d'octets bruts : la taille dépend du format réellement
			// négocié (S24_3LE fait 3 octets, pas 4 comme un float).
			captureScratch_.assign(
				static_cast<size_t>(inCh) * frames * bytesPerSample(alsaCaptureFormat_), 0);
			playbackScratch_.assign(
				static_cast<size_t>(outCh) * frames * bytesPerSample(alsaPlaybackFormat_), 0);
		}

		enum class Recovery { Recovered, Fatal };
		enum class StreamKind { Input, Output };

		Recovery recoverFromError(snd_pcm_t* handle, int errCode, StreamKind kind, const char* streamLabel) {
			if (errCode == -EPIPE) {
				if (kind == StreamKind::Input)
					inputXrunCount_.fetch_add(1, std::memory_order_relaxed);
				else
					outputXrunCount_.fetch_add(1, std::memory_order_relaxed);
				snd_pcm_prepare(handle);
				return Recovery::Recovered;
			}
			if (errCode == -ESTRPIPE) {
				suspendRecoveryCount_.fetch_add(1, std::memory_order_relaxed);
				int attempts = 0;
				int resumeResult;
				while ((resumeResult = snd_pcm_resume(handle)) == -EAGAIN && attempts < 200) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					++attempts;
				}
				if (resumeResult < 0) {
					snd_pcm_prepare(handle);
				}
				return Recovery::Recovered;
			}
			std::string msg = std::string("ALSA fatal error on ") + streamLabel
				+ " stream: " + snd_strerror(errCode);
			setLastError(Result{ Error::GenericError, msg });
			return Recovery::Fatal;
		}

		void audioLoop() {
			// Priorité RT vérifiée, pas juste tentée dans le vide.
			{
				sched_param sch{};
				sch.sched_priority = 50;
				const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch);
				realtimeThreadActive_.store(rc == 0, std::memory_order_relaxed);
			}

			// Verrouille les pages mémoire du process pour éviter les page
			// faults en plein rendu audio. Best-effort, non bloquant en cas
			// d'échec (permissions insuffisantes = comportement dégradé).
			{
				const int rc = mlockall(MCL_CURRENT | MCL_FUTURE);
				memoryLocked_.store(rc == 0, std::memory_order_relaxed);
			}

#if defined(__x86_64__) || defined(__i386__)
			// Flush-to-zero / denormals-are-zero : protège n'importe quel
			// callback utilisateur contre les CPU spikes dus aux dénormaux
			// (ex: queue de reverb qui décroît vers zéro). x86/x64 uniquement.
			_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
			_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

			pthread_setname_np(pthread_self(), "mka-audio-rt");

			const uint32_t inCh   = info_.inputChannels;
			const uint32_t outCh  = info_.outputChannels;
			const uint32_t frames = info_.bufferSize;
			const size_t inBps  = bytesPerSample(alsaCaptureFormat_);
			const size_t outBps = bytesPerSample(alsaPlaybackFormat_);

			while (running_.load(std::memory_order_acquire)) {

				if (captureHandle_) {
					snd_pcm_sframes_t got = snd_pcm_readi(captureHandle_, captureScratch_.data(), frames);
					if (got < 0) {
						if (recoverFromError(captureHandle_, static_cast<int>(got),
						                       StreamKind::Input, "capture") == Recovery::Fatal) {
							running_.store(false, std::memory_order_release);
							state_.store(State::Open, std::memory_order_release);
							break;
						}
						continue;
					}

					for (uint32_t f = 0; f < static_cast<uint32_t>(got); ++f) {
						for (uint32_t c = 0; c < inCh; ++c) {
							const uint8_t* src = captureScratch_.data()
								+ (static_cast<size_t>(f) * inCh + c) * inBps;
							inputChannelPtrs_[c][f] = pcmToFloat(src, alsaCaptureFormat_);
						}
					}
				}

				if (callback_) {
					Buffer buffer{
						.inputs      = inputChannelPtrs_.data(),
						.outputs     = outputChannelPtrs_.data(),
						.inputCount  = inCh,
						.outputCount = outCh,
						.frames      = frames,
					};
					try {
						callback_(buffer, userData_);
					} catch (const std::exception& e) {
						setLastError(Result{ Error::GenericError,
							std::string("Exception in audio callback: ") + e.what() });
					} catch (...) {
						setLastError(Result{ Error::GenericError,
							"Unknown exception in audio callback." });
					}
				}

				if (playbackHandle_) {
					for (uint32_t f = 0; f < frames; ++f) {
						for (uint32_t c = 0; c < outCh; ++c) {
							uint8_t* dst = playbackScratch_.data()
								+ (static_cast<size_t>(f) * outCh + c) * outBps;
							floatToPcm(outputChannelPtrs_[c][f], dst, alsaPlaybackFormat_);
						}
					}

					snd_pcm_sframes_t wrote = snd_pcm_writei(playbackHandle_, playbackScratch_.data(), frames);
					if (wrote < 0) {
						if (recoverFromError(playbackHandle_, static_cast<int>(wrote),
						                       StreamKind::Output, "playback") == Recovery::Fatal) {
							running_.store(false, std::memory_order_release);
							state_.store(State::Open, std::memory_order_release);
							break;
						}
					}
				}
			}

			if (memoryLocked_.load(std::memory_order_relaxed)) {
				munlockall();
				memoryLocked_.store(false, std::memory_order_relaxed);
			}
		}

		Result stopNoLock() {
			running_.store(false, std::memory_order_release);
			if (audioThread_.joinable()) {
				audioThread_.join();
			}
			// Ne repasse Running -> Open que si on était vraiment en train de
			// tourner ; compare_exchange évite d'écraser un état Closed si
			// closeNoLock() a déjà avancé entre-temps.
			State expected = State::Running;
			state_.compare_exchange_strong(expected, State::Open, std::memory_order_acq_rel);
			return mka::audio::Ok;
		}

		Result closeNoLock() {
			if (state_.load(std::memory_order_acquire) == State::Closed) {
				return mka::audio::Ok;
			}
			stopNoLock();

			if (captureHandle_)  { snd_pcm_close(captureHandle_);  captureHandle_ = nullptr; }
			if (playbackHandle_) { snd_pcm_close(playbackHandle_); playbackHandle_ = nullptr; }

			inputPlanar_.clear();
			outputPlanar_.clear();
			inputChannelPtrs_.clear();
			outputChannelPtrs_.clear();
			captureScratch_.clear();
			playbackScratch_.clear();

			state_.store(State::Closed, std::memory_order_release);
			return mka::audio::Ok;
		}

		// Nombre de périodes du ring buffer ALSA. 2 = latence minimale sûre
		// (double-buffering). Pas exposé dans DeviceConfig : pas de besoin
		// prouvé pour l'instant, donc pas d'ajout à l'interface abstraite.
		static constexpr unsigned int kPeriodsCount = 2;

		snd_pcm_t* captureHandle_  = nullptr;
		snd_pcm_t* playbackHandle_ = nullptr;

		snd_pcm_format_t alsaCaptureFormat_  = SND_PCM_FORMAT_FLOAT_LE;
		snd_pcm_format_t alsaPlaybackFormat_ = SND_PCM_FORMAT_FLOAT_LE;

		std::vector<float> inputPlanar_;
		std::vector<float> outputPlanar_;
		std::vector<float*> inputChannelPtrs_;
		std::vector<float*> outputChannelPtrs_;
		std::vector<uint8_t> captureScratch_;
		std::vector<uint8_t> playbackScratch_;

		std::atomic<bool>   running_{false};
		std::atomic<size_t> inputXrunCount_{0};
		std::atomic<size_t> outputXrunCount_{0};
		std::atomic<size_t> suspendRecoveryCount_{0};
		std::atomic<bool>   realtimeThreadActive_{false};
		std::atomic<bool>   memoryLocked_{false};
		std::thread audioThread_;

		std::mutex controlMutex_;

		mutable std::mutex lastErrorMutex_;
		Result lastError_ = mka::audio::Ok;
	};
}
