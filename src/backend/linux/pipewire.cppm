module;
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/dict.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <thread>
#include <vector>

export module mka.audio.backend.pipewire;

import mka.audio.backend.abstract;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.constants;
import mka.audio.process;

namespace mka::audio {
    export class PipeWire final : public Backend {
    public:
        PipeWire() noexcept {
            pw_init(nullptr, nullptr);
        }

        ~PipeWire() override {
            teardownStream();
            pw_deinit();
        }

    protected:
        [[nodiscard]] std::vector<Endpoint> getEndPoints_() const override;

        [[nodiscard]] Result open_(EndpointConfig const &endpointCfg) override;

        [[nodiscard]] Result start_() override;

        [[nodiscard]] Result stop_() override;

        [[nodiscard]] Result close_() override;

    private:
        static void onStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error);

        static void onProcess(void *data);

        void teardownStream() noexcept;

        void waitProcessQuiescent() noexcept;

        pw_thread_loop *loop_ = nullptr;
        pw_stream *stream_ = nullptr;

        Direction direction_ = Direction::Output;
        std::uint32_t channels_ = 0;

        // Préalloués dans open_ : onProcess ne doit jamais allouer.
        std::vector<const float *> inChannels_;
        std::vector<float *> outChannels_;
        std::atomic<bool> ready_ = false;
        std::atomic<bool> failed_ = false;
        std::atomic<bool> active_ = false;

        // Poignée de main de quiescence (style Dekker), les DEUX côtés en seq_cst :
        //  - onProcess : inProcess_ = true ; puis lit processEnabled_ ;
        //  - stop_/rollback : processEnabled_ = false ; puis attend inProcess_ == false.
        // Ainsi, soit onProcess voit processEnabled_ == false (et n'appelle pas le
        // callback), soit le thread de contrôle voit inProcess_ == true et attend.
        // Garantit : plus aucun appel du callback une fois stop() revenu.
        alignas(64) std::atomic<bool> processEnabled_ = false;
        alignas(64) std::atomic<bool> inProcess_ = false;
    };

    namespace {
        // Efface inProcess_ sur tous les chemins de sortie de onProcess.
        struct InProcessGuard {
            std::atomic<bool> &flag;

            ~InProcessGuard() { flag.store(false, std::memory_order_release); }
        };

        constexpr std::chrono::seconds kStartTimeout{5};
        constexpr std::chrono::seconds kStopTimeout{2};
    }

    // --- Callbacks stream -------------------------------------------------

    void PipeWire::onStateChanged(void *data, pw_stream_state /*old*/, pw_stream_state state, const char * /*error*/) {
        auto *self = static_cast<PipeWire *>(data);
        switch (state) {
            case PW_STREAM_STATE_STREAMING:
                self->ready_.store(true, std::memory_order_release);
                // Un STREAMING en retard, traité après stop_(), ne doit pas réarmer active_.
                if (self->processEnabled_.load(std::memory_order_seq_cst)) {
                    self->active_.store(true, std::memory_order_release);
                }
                break;
            case PW_STREAM_STATE_ERROR:
                self->failed_.store(true, std::memory_order_release);
                if (self->active_.load(std::memory_order_acquire)) self->notifyFailed();
                break;
            case PW_STREAM_STATE_UNCONNECTED:   // serveur parti ou nœud supprimé
                if (self->active_.load(std::memory_order_acquire)) self->notifyFailed();
                break;
            default:
                break;
        }
        // Réveille start_()/stop_() qui attendent une transition d'état.
        pw_thread_loop_signal(self->loop_, false);
    }

    void PipeWire::onProcess(void *data) {
        auto *self = static_cast<PipeWire *>(data);

        self->inProcess_.store(true, std::memory_order_seq_cst);
        const InProcessGuard guard{self->inProcess_};
        const bool enabled = self->processEnabled_.load(std::memory_order_seq_cst);

        pw_buffer *b = pw_stream_dequeue_buffer(self->stream_);
        if (!b) {
            if (enabled) self->notifyXRun();
            return;
        }

        spa_buffer *buf = b->buffer;

        // Arrêt en cours : on rend le buffer (silence en sortie), sans callback.
        if (!enabled) {
            if (self->direction_ == Direction::Output) {
                for (std::uint32_t i = 0; i < buf->n_datas; ++i) {
                    spa_data &d = buf->datas[i];
                    if (!d.chunk) continue;
                    d.chunk->offset = 0;
                    d.chunk->stride = sizeof(float);
                    d.chunk->size = 0;
                }
            }
            pw_stream_queue_buffer(self->stream_, b);
            return;
        }

        // F32P = un plan par canal : on attend exactement channels_ plans.
        const bool layoutOk = buf->n_datas == self->channels_;

        if (self->direction_ == Direction::Input) {
            std::uint32_t frames = 0;
            bool ok = layoutOk;

            if (ok) {
                frames = UINT32_MAX;
                for (std::uint32_t i = 0; i < self->channels_; ++i) {
                    const spa_data &d = buf->datas[i];
                    if (!d.data || !d.chunk) {
                        ok = false;
                        break;
                    }

                    const std::uint32_t off = std::min(d.chunk->offset, d.maxsize);
                    const std::uint32_t size = std::min(d.chunk->size, d.maxsize - off);

                    self->inChannels_[i] = reinterpret_cast<const float *>(
                        static_cast<const std::byte *>(d.data) + off);
                    // Minimum sur les canaux : chaque plan est valide sur `frames` échantillons.
                    frames = std::min<std::uint32_t>(frames, size / sizeof(float));
                }
            }
            if (!ok) {
                frames = 0;
                self->notifyXRun();
            }

            if (ok && frames > 0 && self->callback) {
                const AudioProcessContext ctx{
                    .input = {self->inChannels_.data(), self->channels_},
                    .output = {nullptr, 0},
                    .frames = frames
                };
                self->callback(self->userData, ctx);
            }
        } else {
            std::uint32_t frames = 0;
            bool ok = layoutOk;

            if (ok) {
                frames = UINT32_MAX;
                for (std::uint32_t i = 0; i < self->channels_; ++i) {
                    const spa_data &d = buf->datas[i];
                    if (!d.data) {
                        ok = false;
                        break;
                    }
                    frames = std::min<std::uint32_t>(frames, d.maxsize / sizeof(float));
                    self->outChannels_[i] = static_cast<float *>(d.data);
                }
                // Quantum demandé par le graphe (0 = pas d'information).
                if (ok && b->requested != 0) {
                    frames = std::min<std::uint32_t>(frames, static_cast<std::uint32_t>(b->requested));
                }
            }
            if (!ok) {
                frames = 0;
                self->notifyXRun();
            }

            if (frames > 0) {
                // B7 : sortie à zéro avant le callback.
                for (std::uint32_t i = 0; i < self->channels_; ++i) {
                    std::memset(self->outChannels_[i], 0, frames * sizeof(float));
                }

                if (self->callback) {
                    const AudioProcessContext ctx{
                        .input = {nullptr, 0},
                        .output = {self->outChannels_.data(), self->channels_},
                        .frames = frames
                    };
                    self->callback(self->userData, ctx);
                }
            }

            for (std::uint32_t i = 0; i < buf->n_datas; ++i) {
                spa_data &d = buf->datas[i];
                if (!d.chunk) continue;
                d.chunk->offset = 0;
                d.chunk->stride = sizeof(float);
                d.chunk->size = frames * static_cast<std::uint32_t>(sizeof(float));
            }
        }

        pw_stream_queue_buffer(self->stream_, b);
    }

    // --- open_ / start_ / stop_ / close_ -----------------------------------

    Result PipeWire::open_(EndpointConfig const &endpointCfg) {
        if (endpointCfg.direction == Direction::Duplex) {
            return std::unexpected{ErrorType::ConfigurationFailed};
        }

        const auto contains = [](auto const &arr, auto value) {
            return std::find(arr.begin(), arr.end(), value) != arr.end();
        };
        if (!contains(supportedSampleRates, endpointCfg.sampleRate))
            return std::unexpected{ErrorType::SampleRateNotSupported};
        if (!contains(supportedBufferSizes, endpointCfg.bufferSize))
            return std::unexpected{ErrorType::BufferSizeNotSupported};

        if (endpointCfg.format != Format::Float32)
            return std::unexpected{ErrorType::FormatNotSupported};

        const std::uint32_t channels = endpointCfg.direction == Direction::Input
                                           ? endpointCfg.inputChannels
                                           : endpointCfg.outputChannels;
        if (channels == 0)
            return std::unexpected{ErrorType::ChannelsNotSupported};

        channels_ = channels;
        inChannels_.assign(channels, nullptr);
        outChannels_.assign(channels, nullptr);

        if (!endpointCfg.id.empty()) {
            const auto endpoints = getEndPoints_();
            const auto it = std::ranges::find_if(endpoints, [&](Endpoint const &e) {
                return e.id == endpointCfg.id;
            });
            const bool hasCapability = it != endpoints.end() && (
                                           endpointCfg.direction == Direction::Input
                                               ? it->input.has_value()
                                               : it->output.has_value());
            if (!hasCapability)
                return std::unexpected{ErrorType::EndpointUnavailable};
        }

        direction_ = endpointCfg.direction;

        // Construit AVANT toute ressource native : to_string peut lever.
        const std::string latency = std::to_string(endpointCfg.bufferSize) + "/" +
                                    std::to_string(endpointCfg.sampleRate);

        loop_ = pw_thread_loop_new("mka-audio-pipewire", nullptr);
        if (!loop_) return std::unexpected{ErrorType::ConfigurationFailed};

        pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, direction_ == Direction::Input ? "Capture" : "Playback",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_NODE_LATENCY,
            latency.c_str(),
            nullptr);

        if (!props) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return std::unexpected{ErrorType::ConfigurationFailed};
        }

        if (!endpointCfg.id.empty()) {
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, endpointCfg.id.c_str());
        }

        static constexpr pw_stream_events streamEvents = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = &PipeWire::onStateChanged,
            .process = &PipeWire::onProcess,
        };

        // pw_stream_new_simple consomme `props` et gère en interne le
        // contexte/la core associés à `loop_` : pas besoin de les garder.
        stream_ = pw_stream_new_simple(
            pw_thread_loop_get_loop(loop_),
            "mka-audio-stream",
            props,
            &streamEvents,
            this);

        if (!stream_) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return std::unexpected{ErrorType::EndpointUnavailable};
        }

        std::uint8_t podBuffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32P;
        info.channels = channels;
        info.rate = endpointCfg.sampleRate;

        const spa_pod *params[1] = {
            spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)
        };

        const auto pwDirection = direction_ == Direction::Input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT;
        const auto flags = static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

        if (pw_stream_connect(stream_, pwDirection, PW_ID_ANY, flags, params, 1) < 0) {
            pw_stream_destroy(stream_);
            stream_ = nullptr;
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return std::unexpected{ErrorType::ConfigurationFailed};
        }

        return {};
    }

    void PipeWire::waitProcessQuiescent() noexcept {
        while (inProcess_.load(std::memory_order_seq_cst)) std::this_thread::yield();
    }

    Result PipeWire::start_() {
        ready_.store(false, std::memory_order_relaxed);
        failed_.store(false, std::memory_order_relaxed);

        pw_thread_loop_lock(loop_);

        if (pw_thread_loop_start(loop_) < 0) {
            pw_thread_loop_unlock(loop_);
            return std::unexpected{ErrorType::ConfigurationFailed};
        }

        // callback/userData sont figés tant que le backend est Open/Running.
        processEnabled_.store(true, std::memory_order_seq_cst);

        if (pw_stream_set_active(stream_, true) < 0) {
            processEnabled_.store(false, std::memory_order_seq_cst);
            pw_stream_set_active(stream_, false);
            pw_thread_loop_unlock(loop_);
            waitProcessQuiescent();
            pw_thread_loop_stop(loop_);
            return std::unexpected{ErrorType::ConfigurationFailed};
        }

        // Attente bornée. On lit aussi l'état réel du stream : après un redémarrage,
        // un stream déjà STREAMING/ERROR n'émet plus de state_changed.
        const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
        bool isReady = false;
        bool hasFailed = false;

        while (true) {
            const pw_stream_state st = pw_stream_get_state(stream_, nullptr);
            if (st == PW_STREAM_STATE_STREAMING || ready_.load(std::memory_order_acquire)) {
                isReady = true;
                break;
            }
            if (st == PW_STREAM_STATE_ERROR || st == PW_STREAM_STATE_UNCONNECTED ||
                failed_.load(std::memory_order_acquire)) {
                hasFailed = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            pw_thread_loop_timed_wait(loop_, 1);
        }

        if (isReady) {
            active_.store(true, std::memory_order_release);
            pw_thread_loop_unlock(loop_);
            return {};
        }

        // Échec ou délai dépassé : retour arrière complet.
        processEnabled_.store(false, std::memory_order_seq_cst);
        pw_stream_set_active(stream_, false);
        pw_thread_loop_unlock(loop_);
        waitProcessQuiescent();
        pw_thread_loop_stop(loop_);

        return std::unexpected{hasFailed ? ErrorType::ConfigurationFailed : ErrorType::EndpointUnavailable};
    }

    Result PipeWire::stop_() {
        // D'abord interdire le callback (voir poignée de main dans la classe).
        processEnabled_.store(false, std::memory_order_seq_cst);
        active_.store(false, std::memory_order_release);
        if (stream_) {
            pw_thread_loop_lock(loop_);
            pw_stream_set_active(stream_, false);
            // Attente bornée de la sortie de STREAMING : sinon, au start_() suivant,
            // l'état périmé ferait passer le démarrage pour immédiat.
            const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
            while (pw_stream_get_state(stream_, nullptr) == PW_STREAM_STATE_STREAMING &&
                   std::chrono::steady_clock::now() < deadline) {
                pw_thread_loop_timed_wait(loop_, 1);
            }
            pw_thread_loop_unlock(loop_);
        }
        // Attend la fin d'un éventuel onProcess en cours avant d'arrêter la boucle.
        waitProcessQuiescent();
        if (loop_) {
            // Demande l'arrêt de la boucle/thread audio.
            pw_thread_loop_stop(loop_);
        }
        return {};
    }

    Result PipeWire::close_() {
        teardownStream();
        return {};
    }

    void PipeWire::teardownStream() noexcept {
        // Le destructeur peut s'exécuter alors que le backend est Running.
        processEnabled_.store(false, std::memory_order_seq_cst);
        active_.store(false, std::memory_order_release);
        // Sans danger si la boucle est déjà arrêtée.
        if (loop_) pw_thread_loop_stop(loop_);
        if (stream_) {
            // Boucle arrêtée : plus de thread audio, pas besoin de verrou.
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (loop_) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
    }

    // --- getEndPoints_ ------------------------------------------------------

    namespace {
        struct ScanContext {
            pw_thread_loop *loop = nullptr;
            std::vector<Endpoint> endpoints;
            int pendingSync = -1;
            bool done = false;
        };

        void onRegistryGlobal(void *data, std::uint32_t id, std::uint32_t /*permissions*/,
                              const char *type, std::uint32_t /*version*/, const spa_dict *props) {
            auto *ctx = static_cast<ScanContext *>(data);
            if (!props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

            const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            if (!mediaClass) return;

            const bool isSource = std::strcmp(mediaClass, "Audio/Source") == 0;
            const bool isSink = std::strcmp(mediaClass, "Audio/Sink") == 0;
            if (!isSource && !isSink) return;

            const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

            Endpoint ep;
            ep.id = name ? name : std::to_string(id);
            ep.name = desc ? desc : ep.id;

            // Capacités génériques (voir note en tête de fichier) : pas de
            // requête fine SPA_PARAM_EnumFormat par noeud pour l'instant.
            StreamCapabilities caps{
                .minChannels = 1,
                .maxChannels = 2,
                .sampleRates = {supportedSampleRates.begin(), supportedSampleRates.end()},
                .formats = {Format::Float32},
                .bufferSizes = {supportedBufferSizes.begin(), supportedBufferSizes.end()},
            };

            if (isSource) ep.input = caps;
            else ep.output = caps;

            ctx->endpoints.push_back(std::move(ep));
        }

        void onCoreDone(void *data, std::uint32_t id, int seq) {
            auto *ctx = static_cast<ScanContext *>(data);
            if (id == PW_ID_CORE && seq == ctx->pendingSync) {
                ctx->done = true;
                pw_thread_loop_signal(ctx->loop, false);
            }
        }
    }

    std::vector<Endpoint> PipeWire::getEndPoints_() const {
        try {
            pw_thread_loop *scanLoop = pw_thread_loop_new("mka-audio-pipewire-scan", nullptr);
            if (!scanLoop) return {};

            pw_context *context = pw_context_new(pw_thread_loop_get_loop(scanLoop), nullptr, 0);
            if (!context) {
                pw_thread_loop_destroy(scanLoop);
                return {};
            }

            pw_core *core = pw_context_connect(context, nullptr, 0);
            if (!core) {
                pw_context_destroy(context);
                pw_thread_loop_destroy(scanLoop);
                return {};
            }

            pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
            if (!registry) {
                pw_core_disconnect(core);
                pw_context_destroy(context);
                pw_thread_loop_destroy(scanLoop);
                return {};
            }

            ScanContext ctx;
            ctx.loop = scanLoop;

            static const pw_registry_events registryEvents = {
                .version = PW_VERSION_REGISTRY_EVENTS,
                .global = &onRegistryGlobal,
            };
            static const pw_core_events coreEvents = {
                .version = PW_VERSION_CORE_EVENTS,
                .done = &onCoreDone,
            };

            spa_hook registryListener{};
            spa_hook coreListener{};
            pw_registry_add_listener(registry, &registryListener, &registryEvents, &ctx);
            pw_core_add_listener(core, &coreListener, &coreEvents, &ctx);

            pw_thread_loop_lock(scanLoop);
            if (pw_thread_loop_start(scanLoop) == 0) {
                ctx.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
                while (!ctx.done) {
                    pw_thread_loop_wait(scanLoop);
                }
            }
            pw_thread_loop_unlock(scanLoop);

            pw_thread_loop_stop(scanLoop);
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
            pw_core_disconnect(core);
            pw_context_destroy(context);
            pw_thread_loop_destroy(scanLoop);

            return std::move(ctx.endpoints);
        } catch (...) {
            return {};
        }
    }
}
