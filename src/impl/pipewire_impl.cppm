module;
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

export module audio.pipewire;
import audio.abstract_core;
import audio.error;
import audio.config;
import audio.constants;

namespace mka::audio {

	std::once_flag pwInitFlag;
	void ensurePipeWireInitialized() {
		std::call_once(pwInitFlag, []() { pw_init(nullptr, nullptr); });
	}

	// Ring buffer mono-canal protégé par mutex : fait transiter les
	// échantillons capturés (callback capture async) vers le callback
	// playback qui sert d'horloge principale en duplex. Même compromis
	// que sur PulseAudio : une latence de synchronisation supplémentaire
	// (généralement 1-2 périodes) par rapport à un flux réellement
	// synchrone comme ALSA readi/writei dans la même boucle.
	//
	// Non copiable/non déplaçable à cause du mutex : stocké derrière
	// unique_ptr dans un vector pour permettre resize() sans déplacer
	// l'objet lui-même (seul le pointeur bouge).
	class ChannelRing {
	public:
		ChannelRing() = default;
		ChannelRing(const ChannelRing&) = delete;
		ChannelRing& operator=(const ChannelRing&) = delete;
		ChannelRing(ChannelRing&&) = delete;
		ChannelRing& operator=(ChannelRing&&) = delete;

		void reset(size_t capacityFrames) {
			std::lock_guard lock(mutex_);
			buffer_.assign(capacityFrames, 0.0f);
			writePos_ = 0;
			available_ = 0;
		}
		void push(const float* data, size_t frames) {
			std::lock_guard lock(mutex_);
			for (size_t i = 0; i < frames; ++i) {
				buffer_[writePos_] = data[i];
				writePos_ = (writePos_ + 1) % buffer_.size();
			}
			available_ = std::min(available_ + frames, buffer_.size());
		}
		size_t pop(float* out, size_t frames) {
			std::lock_guard lock(mutex_);
			const size_t toRead = std::min(frames, available_);
			size_t readPos = (writePos_ + buffer_.size() - available_) % buffer_.size();
			for (size_t i = 0; i < toRead; ++i) {
				out[i] = buffer_[readPos];
				readPos = (readPos + 1) % buffer_.size();
			}
			available_ -= toRead;
			return toRead;
		}
	private:
		std::mutex mutex_;
		std::vector<float> buffer_;
		size_t writePos_ = 0;
		size_t available_ = 0;
	};

	// deviceID est optionnel : nom de node PipeWire (obtenu via
	// enumerateDevices()) pour cibler un device précis, ou vide pour laisser
	// WirePlumber choisir le sink/source par défaut du système.
	//
	// IMPORTANT : cette implémentation utilise l'API pw_stream (pas
	// pw_filter). C'est le chemin que WirePlumber sait router automatiquement
	// de façon fiable — c'est exactement ce que font pw-play, mpv, ou tout
	// navigateur. pw_filter crée des noeuds visibles dans le graphe mais QUE
	// WirePlumber ne relie jamais automatiquement au sink par défaut, quelles
	// que soient les properties posées dessus.
	//
	// Pour router différentes pistes vers différents COUPLES DE SORTIE d'UNE
	// MÊME interface (ex: 1/2 vs 3/4), ouvre un seul PipeWire avec
	// outputChannels = nombre total de canaux physiques, et écris chaque
	// piste dans le sous-ensemble de buffer.outputs[] qui lui correspond.
	//
	// Pour router vers un DEVICE PHYSIQUE DIFFÉRENT (ex: interface principale
	// + DAC casque séparé), ouvre une DEUXIÈME instance PipeWire avec un
	// deviceID différent.
	export class PipeWire final : public Device {
	public:
		PipeWire() = default;
		~PipeWire() override { closeNoLock(); }

		struct DeviceDescriptor {
			std::string nodeName;
			std::string description;
			bool isSink   = false;
			bool isSource = false;
		};

		[[nodiscard]] static std::vector<DeviceDescriptor> enumerateDevices() {
			ensurePipeWireInitialized();
			std::vector<DeviceDescriptor> result;

			pw_main_loop* loop = pw_main_loop_new(nullptr);
			pw_context* context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
			pw_core* core = pw_context_connect(context, nullptr, 0);
			if (!core) {
				pw_context_destroy(context);
				pw_main_loop_destroy(loop);
				return result;
			}

			pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

			struct EnumState {
				pw_main_loop* loop;
				std::vector<DeviceDescriptor>* out;
			} state{ loop, &result };

			spa_hook registryListener{};
			pw_registry_events registryEvents{};
			registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
			registryEvents.global = [](void* data, uint32_t, uint32_t, const char* type,
			                             uint32_t, const spa_dict* props) {
				if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
				const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
				if (!mediaClass) return;
				const bool isSink   = std::strstr(mediaClass, "Audio/Sink")   != nullptr;
				const bool isSource = std::strstr(mediaClass, "Audio/Source") != nullptr;
				if (!isSink && !isSource) return;
				auto* st = static_cast<EnumState*>(data);
				const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
				const char* nodeDesc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
				st->out->push_back({
					.nodeName    = nodeName ? nodeName : "",
					.description = nodeDesc ? nodeDesc : (nodeName ? nodeName : "Unknown device"),
					.isSink      = isSink,
					.isSource    = isSource
				});
			};
			pw_registry_add_listener(registry, &registryListener, &registryEvents, &state);

			spa_hook coreListener{};
			pw_core_events coreEvents{};
			coreEvents.version = PW_VERSION_CORE_EVENTS;
			coreEvents.done = [](void* data, uint32_t, int) {
				pw_main_loop_quit(static_cast<EnumState*>(data)->loop);
			};
			pw_core_add_listener(core, &coreListener, &coreEvents, &state);
			pw_core_sync(core, PW_ID_CORE, 0);
			pw_main_loop_run(loop);

			spa_hook_remove(&registryListener);
			spa_hook_remove(&coreListener);
			pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
			pw_core_disconnect(core);
			pw_context_destroy(context);
			pw_main_loop_destroy(loop);
			return result;
		}

		// Sonde légère : teste uniquement la présence d'un serveur PipeWire
		// joignable, sans créer de flux ni négocier de format. Utile pour un
		// fallback vers ALSA sans passer par un open() complet.
		[[nodiscard]] static bool isServerReachable() {
			ensurePipeWireInitialized();
			pw_main_loop* loop = pw_main_loop_new(nullptr);
			pw_context* context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
			pw_core* core = pw_context_connect(context, nullptr, 0);
			const bool reachable = (core != nullptr);
			if (core) pw_core_disconnect(core);
			pw_context_destroy(context);
			pw_main_loop_destroy(loop);
			return reachable;
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

		// NOTE : pw_stream n'expose pas de compteur d'xrun direct et fiable
		// dans cette API. Retourne toujours 0, documenté honnêtement.
		[[nodiscard]] size_t xrunCount() const noexcept {
			return xrunCount_.load(std::memory_order_relaxed);
		}

		[[nodiscard]] Result lastError() const {
			std::lock_guard lock(lastErrorMutex_);
			return lastError_;
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

			ensurePipeWireInitialized();

			loop_ = pw_thread_loop_new("mka-pw-control", nullptr);
			if (!loop_) {
				return Result{ Error::DeviceOpenFailed, "pw_thread_loop_new failed." };
			}
			pw_thread_loop_lock(loop_);

			readyPlayback_.store(cfg.outputChannels == 0, std::memory_order_release);
			readyCapture_.store(cfg.inputChannels == 0, std::memory_order_release);
			streamError_.store(false, std::memory_order_release);

			if (cfg.outputChannels > 0) {
				if (auto r = createStream(playbackStream_, PW_DIRECTION_OUTPUT, cfg,
				                            cfg.outputChannels, "mka_audio_out"); !r) {
					pw_thread_loop_unlock(loop_);
					closeNoLock();
					return r;
				}
			}
			if (cfg.inputChannels > 0) {
				if (auto r = createStream(captureStream_, PW_DIRECTION_INPUT, cfg,
				                            cfg.inputChannels, "mka_audio_in"); !r) {
					pw_thread_loop_unlock(loop_);
					closeNoLock();
					return r;
				}
			}

			if (pw_thread_loop_start(loop_) < 0) {
				pw_thread_loop_unlock(loop_);
				closeNoLock();
				return Result{ Error::DeviceOpenFailed, "pw_thread_loop_start failed." };
			}

			while (!readyPlayback_.load(std::memory_order_acquire)
			       && !streamError_.load(std::memory_order_acquire)) {
				pw_thread_loop_wait(loop_);
			}
			while (!readyCapture_.load(std::memory_order_acquire)
			       && !streamError_.load(std::memory_order_acquire)) {
				pw_thread_loop_wait(loop_);
			}
			const bool failed = streamError_.load(std::memory_order_acquire);
			pw_thread_loop_unlock(loop_);

			if (failed) {
				std::string msg = negotiationError_;
				closeNoLock();
				return Result{ Error::DeviceOpenFailed, "PipeWire stream negotiation failed: " + msg };
			}

			info_.id              = cfg.deviceID;
			info_.name             = "mkaudio"; //cfg.deviceID.empty() ? "PipeWire (default)" : cfg.deviceID;
			info_.sampleRate       = cfg.sampleRate;
			info_.bufferSize       = cfg.bufferSize;
			info_.inputChannels    = cfg.inputChannels;
			info_.outputChannels   = cfg.outputChannels;
			const double periodMs = 1000.0 * cfg.bufferSize / cfg.sampleRate;
			info_.inputLatencyMs  = cfg.inputChannels  > 0 ? periodMs * 2.0 : 0.0;
			info_.outputLatencyMs = cfg.outputChannels > 0 ? periodMs * 2.0 : 0.0;
			info_.sampleFormat = SampleFormat::Float32;

			try {
				inputChannelBuffers_.assign(cfg.inputChannels, nullptr);
				outputChannelBuffers_.assign(cfg.outputChannels, nullptr);
				inputPlanarScratch_.assign(static_cast<size_t>(cfg.inputChannels) * cfg.bufferSize, 0.0f);
				outputPlanarScratch_.assign(static_cast<size_t>(cfg.outputChannels) * cfg.bufferSize, 0.0f);
				for (uint32_t c = 0; c < cfg.inputChannels; ++c)
					inputChannelBuffers_[c] = &inputPlanarScratch_[static_cast<size_t>(c) * cfg.bufferSize];
				for (uint32_t c = 0; c < cfg.outputChannels; ++c)
					outputChannelBuffers_[c] = &outputPlanarScratch_[static_cast<size_t>(c) * cfg.bufferSize];

				captureRings_.clear();
				if (cfg.inputChannels > 0 && cfg.outputChannels > 0) {
					// unique_ptr par ring : ChannelRing contient un mutex non
					// copiable/déplaçable, donc le vector ne peut stocker que
					// des pointeurs, jamais les objets par valeur.
					captureRings_.reserve(cfg.inputChannels);
					for (uint32_t c = 0; c < cfg.inputChannels; ++c) {
						auto ring = std::make_unique<ChannelRing>();
						ring->reset(cfg.bufferSize * 4);
						captureRings_.push_back(std::move(ring));
					}
				}
			} catch (const std::exception&) {
				closeNoLock();
				return Result{ Error::GenericError, "Failed to allocate audio buffers (out of memory)." };
			}

			configured_ = { cfg.inputChannels, cfg.outputChannels, cfg.bufferSize };

			clearLastError();
			xrunCount_.store(0, std::memory_order_relaxed);

			state_.store(State::Open, std::memory_order_release);
			return mka::audio::Ok;
		}

		[[nodiscard]] Result createStream(pw_stream*& stream, spa_direction dir,
		                                    const DeviceConfig& cfg, uint32_t channels,
		                                    const char* streamName) {
			// media.class = "Stream/Output/Audio" ou "Stream/Input/Audio" est
			// LA propriété que WirePlumber utilise pour identifier un flux
			// à auto-router.
			pw_properties* props = pw_properties_new(
				PW_KEY_MEDIA_TYPE, "Audio",
				PW_KEY_MEDIA_CATEGORY, dir == PW_DIRECTION_OUTPUT ? "Playback" : "Capture",
				PW_KEY_MEDIA_CLASS, dir == PW_DIRECTION_OUTPUT ? "Stream/Output/Audio" : "Stream/Input/Audio",
				PW_KEY_MEDIA_ROLE, "Production",
				PW_KEY_NODE_NAME, streamName,
				nullptr
			);
			if (!cfg.deviceID.empty()) {
				pw_properties_set(props, PW_KEY_TARGET_OBJECT, cfg.deviceID.c_str());
			}

			static const pw_stream_events playbackEvents = makeStreamEvents(true);
			static const pw_stream_events captureEvents  = makeStreamEvents(false);

			stream = pw_stream_new_simple(
				pw_thread_loop_get_loop(loop_),
				streamName,
				props,
				dir == PW_DIRECTION_OUTPUT ? &playbackEvents : &captureEvents,
				this
			);
			if (!stream) {
				return Result{ Error::DeviceOpenFailed, "pw_stream_new_simple failed." };
			}

			uint8_t podBuffer[1024];
			spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
			spa_audio_info_raw audioInfo{};
			audioInfo.format   = SPA_AUDIO_FORMAT_F32;
			audioInfo.channels = channels;
			audioInfo.rate     = cfg.sampleRate;

			const spa_pod* params[1];
			params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audioInfo);

			const auto flags = static_cast<pw_stream_flags>(
				PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS
			);
			if (pw_stream_connect(stream, dir, PW_ID_ANY, flags, params, 1) < 0) {
				pw_stream_destroy(stream);
				stream = nullptr;
				return Result{ Error::DeviceOpenFailed, "pw_stream_connect failed." };
			}
			return mka::audio::Ok;
		}

		[[nodiscard]] Result startNoLock() {
			if (state_.load(std::memory_order_acquire) != State::Open) {
				return Result{ Error::WouldBlock, "Device must be open before starting." };
			}
			// pw_stream commence à produire/consommer dès connect() ; comme
			// sur PulseAudio, start()/stop() basculent un état logique pour
			// respecter le contrat de cycle de vie de l'interface Device.
			clearLastError();
			state_.store(State::Running, std::memory_order_release);
			return mka::audio::Ok;
		}

		Result stopNoLock() {
			if (state_.load(std::memory_order_acquire) != State::Running) {
				return mka::audio::Ok;
			}
			state_.store(State::Open, std::memory_order_release);
			return mka::audio::Ok;
		}

		Result closeNoLock() {
			if (state_.load(std::memory_order_acquire) == State::Closed) {
				return mka::audio::Ok;
			}
			stopNoLock();

			if (loop_) {
				pw_thread_loop_lock(loop_);
				if (playbackStream_) { pw_stream_destroy(playbackStream_); playbackStream_ = nullptr; }
				if (captureStream_)  { pw_stream_destroy(captureStream_);  captureStream_  = nullptr; }
				pw_thread_loop_unlock(loop_);
				pw_thread_loop_stop(loop_);
				pw_thread_loop_destroy(loop_);
				loop_ = nullptr;
			}

			captureRings_.clear();
			inputChannelBuffers_.clear();
			outputChannelBuffers_.clear();
			inputPlanarScratch_.clear();
			outputPlanarScratch_.clear();

			state_.store(State::Closed, std::memory_order_release);
			return mka::audio::Ok;
		}

		struct ConfiguredChannels { uint32_t inputChannels; uint32_t outputChannels; uint32_t bufferSize; };
		ConfiguredChannels configured_{};

		static pw_stream_events makeStreamEvents(bool isPlayback) {
			pw_stream_events events{};
			events.version = PW_VERSION_STREAM_EVENTS;
			events.state_changed = isPlayback ? onPlaybackStateChanged : onCaptureStateChanged;
			events.process       = isPlayback ? onPlaybackProcess : onCaptureProcess;
			return events;
		}

		static void onPlaybackStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error) {
			auto* self = static_cast<PipeWire*>(data);
			if (state == PW_STREAM_STATE_ERROR) {
				self->negotiationError_ = error ? error : "unknown playback stream error";
				self->streamError_.store(true, std::memory_order_release);
				self->setLastError(Result{ Error::GenericError,
					std::string("PipeWire playback error: ") + self->negotiationError_ });
				pw_thread_loop_signal(self->loop_, false);
			} else if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
				self->readyPlayback_.store(true, std::memory_order_release);
				pw_thread_loop_signal(self->loop_, false);
			} else if (state == PW_STREAM_STATE_UNCONNECTED
			           && self->state_.load(std::memory_order_acquire) == State::Running) {
				self->setLastError(Result{ Error::GenericError,
					"PipeWire playback stream unexpectedly disconnected." });
				self->state_.store(State::Open, std::memory_order_release);
			}
		}

		static void onCaptureStateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error) {
			auto* self = static_cast<PipeWire*>(data);
			if (state == PW_STREAM_STATE_ERROR) {
				self->negotiationError_ = error ? error : "unknown capture stream error";
				self->streamError_.store(true, std::memory_order_release);
				self->setLastError(Result{ Error::GenericError,
					std::string("PipeWire capture error: ") + self->negotiationError_ });
				pw_thread_loop_signal(self->loop_, false);
			} else if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
				self->readyCapture_.store(true, std::memory_order_release);
				pw_thread_loop_signal(self->loop_, false);
			} else if (state == PW_STREAM_STATE_UNCONNECTED
			           && self->state_.load(std::memory_order_acquire) == State::Running) {
				self->setLastError(Result{ Error::GenericError,
					"PipeWire capture stream unexpectedly disconnected." });
				self->state_.store(State::Open, std::memory_order_release);
			}
		}

		static void onCaptureProcess(void* data) {
			auto* self = static_cast<PipeWire*>(data);
			pw_buffer* b = pw_stream_dequeue_buffer(self->captureStream_);
			if (!b) return;
			spa_buffer* buf = b->buffer;
			if (!buf->datas[0].data) { pw_stream_queue_buffer(self->captureStream_, b); return; }

			const uint32_t inCh = self->configured_.inputChannels;
			const auto* src = static_cast<const float*>(buf->datas[0].data);
			const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * inCh;
			const uint32_t frames = stride > 0 ? buf->datas[0].chunk->size / stride : 0;

			if (self->configured_.outputChannels > 0) {
				// Duplex : pousse dans les ring buffers, consommés par le
				// callback playback qui sert d'horloge principale.
				static thread_local std::vector<float> deinterleave;
				deinterleave.resize(frames);
				for (uint32_t c = 0; c < inCh; ++c) {
					for (uint32_t f = 0; f < frames; ++f) deinterleave[f] = src[f * inCh + c];
					self->captureRings_[c]->push(deinterleave.data(), frames);
				}
			} else {
				// Capture seule : appelle directement le callback utilisateur.
				static thread_local std::vector<float> deinterleave;
				deinterleave.resize(static_cast<size_t>(inCh) * frames);
				for (uint32_t c = 0; c < inCh; ++c) {
					for (uint32_t f = 0; f < frames; ++f)
						deinterleave[static_cast<size_t>(c) * frames + f] = src[f * inCh + c];
					self->inputChannelBuffers_[c] = &deinterleave[static_cast<size_t>(c) * frames];
				}
				if (self->callback_) {
					Buffer buffer{
						.inputs = self->inputChannelBuffers_.data(), .outputs = nullptr,
						.inputCount = inCh, .outputCount = 0, .frames = frames,
					};
					try { self->callback_(buffer, self->userData_); }
					catch (const std::exception& e) {
						self->setLastError(Result{ Error::GenericError,
							std::string("Exception in audio callback: ") + e.what() });
					} catch (...) {
						self->setLastError(Result{ Error::GenericError, "Unknown exception in audio callback." });
					}
				}
			}
			pw_stream_queue_buffer(self->captureStream_, b);
		}

		static void onPlaybackProcess(void* data) {
			auto* self = static_cast<PipeWire*>(data);
			pw_buffer* b = pw_stream_dequeue_buffer(self->playbackStream_);
			if (!b) return;
			spa_buffer* buf = b->buffer;
			if (!buf->datas[0].data) { pw_stream_queue_buffer(self->playbackStream_, b); return; }

			const uint32_t outCh = self->configured_.outputChannels;
			const uint32_t inCh  = self->configured_.inputChannels;
			const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * outCh;
			const uint32_t maxFrames = stride > 0 ? buf->datas[0].maxsize / stride : 0;
			const uint32_t frames = std::min(maxFrames, self->configured_.bufferSize);

			if (inCh > 0) {
				for (uint32_t c = 0; c < inCh; ++c) {
					const size_t got = self->captureRings_[c]->pop(self->inputChannelBuffers_[c], frames);
					if (got < frames) {
						std::fill(self->inputChannelBuffers_[c] + got, self->inputChannelBuffers_[c] + frames, 0.0f);
					}
				}
			}

			if (self->callback_) {
				Buffer buffer{
					.inputs      = self->inputChannelBuffers_.data(),
					.outputs     = self->outputChannelBuffers_.data(),
					.inputCount  = inCh,
					.outputCount = outCh,
					.frames      = frames,
				};
				try { self->callback_(buffer, self->userData_); }
				catch (const std::exception& e) {
					self->setLastError(Result{ Error::GenericError,
						std::string("Exception in audio callback: ") + e.what() });
				} catch (...) {
					self->setLastError(Result{ Error::GenericError, "Unknown exception in audio callback." });
				}
			}

			auto* dst = static_cast<float*>(buf->datas[0].data);
			for (uint32_t f = 0; f < frames; ++f)
				for (uint32_t c = 0; c < outCh; ++c)
					dst[f * outCh + c] = self->outputChannelBuffers_[c][f];

			buf->datas[0].chunk->offset = 0;
			buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
			buf->datas[0].chunk->size   = frames * stride;
			pw_stream_queue_buffer(self->playbackStream_, b);
		}

		pw_thread_loop* loop_           = nullptr;
		pw_stream* playbackStream_      = nullptr;
		pw_stream* captureStream_       = nullptr;

		std::atomic<bool> readyPlayback_{false};
		std::atomic<bool> readyCapture_{false};
		std::atomic<bool> streamError_{false};
		std::string negotiationError_;

		// vector<unique_ptr<ChannelRing>> : ChannelRing contient un mutex
		// non copiable/déplaçable, donc impossible de le stocker par valeur
		// dans un vector redimensionnable. Chaque ring est alloué
		// individuellement et référencé par pointeur stable.
		std::vector<std::unique_ptr<ChannelRing>> captureRings_;
		std::vector<float*> inputChannelBuffers_;
		std::vector<float*> outputChannelBuffers_;
		std::vector<float> inputPlanarScratch_;
		std::vector<float> outputPlanarScratch_;

		std::atomic<size_t> xrunCount_{0};

		std::mutex controlMutex_;
		mutable std::mutex lastErrorMutex_;
		Result lastError_ = mka::audio::Ok;
	};
}
