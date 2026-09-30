//
// Created by mika on 9/24/26.
//
// Cette version inclut :
//  B1  ProcessFunction avec contexte utilisateur (void* user) ;
//  B8  remontée d'erreurs à l'exécution : compteur d'xruns, drapeau "flux mort",
//      et gestionnaire d'événements appelé HORS du thread audio ;
//  TS  opérations de contrôle sérialisées par un mutex, exceptions des hooks
//      converties en erreurs (les méthodes publiques ne lèvent jamais).
//
module;
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>
export module mka.audio.backend.abstract;
export import mka.audio.error;
export import mka.audio.endpoint;
export import mka.audio.constants;
export import mka.audio.process;

export namespace mka::audio {

    enum class EventType : std::uint8_t {
        // Au moins un xrun (sous/sur-dépassement de buffer) depuis l'événement précédent.
        // Event::xruns = total cumulé depuis start(). Plusieurs xruns rapprochés
        // (< ~10 ms) sont regroupés en un seul événement.
        XRun,
        // Le flux est mort et ne repartira pas seul (serveur audio arrêté, périphérique
        // débranché, erreur de flux). Émis une seule fois par start(). Il n'y a plus
        // d'audio : l'application doit appeler stop() puis close() (et éventuellement
        // rouvrir un endpoint).
        Failed,
    };

    struct Event {
        EventType type;
        std::uint64_t xruns;
    };

    // Le thread audio tourne-t-il en priorité temps réel ?
    //  Yes / No : le backend le sait (ALSA : SCHED_FIFO obtenu ou refusé ;
    //             JACK : le serveur est-il en mode realtime).
    //  Unknown  : le thread appartient à un serveur ou une lib que le backend ne
    //             peut pas interroger (PipeWire, PulseAudio), ou avant start().
    enum class RealtimeState : std::uint8_t { Unknown, No, Yes };

    // Instantané lisible à tout moment, depuis n'importe quel thread.
    struct Status {
        std::uint64_t xruns = 0;   // depuis le dernier start()
        bool failed = false;
        RealtimeState realtime = RealtimeState::Unknown;
    };

    // Appelé depuis un thread dédié, NON temps réel, créé par start().
    // Contraintes : le handler ne doit pas appeler les méthodes du backend
    // (stop() depuis ce thread s'attendrait lui-même) ; il doit plutôt poster un
    // message vers le thread de contrôle de l'application.
    using EventHandler = void (*)(void* user, const Event&) noexcept;

    class Backend {
        public:
            Backend() noexcept = default;
            virtual ~Backend() = default;

            Backend(const Backend&) = delete;
            Backend& operator=(const Backend&) = delete;

            Backend(Backend&&) = delete;
            Backend& operator=(Backend&&) = delete;

            // Toutes les opérations de contrôle sont sérialisées par un mutex : elles peuvent
            // être appelées depuis plusieurs threads sans course sur l'état. Les hooks *_
            // des classes filles peuvent lever des exceptions : elles sont converties en
            // ErrorType::ConfigurationFailed (le backend reste dans son état précédent).
            // Aucune de ces méthodes ne peut être appelée depuis le handler d'événements
            // (elles renvoient InvalidState), sinon stop() s'attendrait lui-même.

            [[nodiscard]] Result open(EndpointConfig const &endpointCfg) {
                if (calledFromDispatcher()) return std::unexpected{ ErrorType::InvalidState };
                std::scoped_lock lock(controlMutex_);

                if (state != State::Closed) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return guarded([&] { return open_(endpointCfg); }).and_then([&]() -> Result {
                    state = State::Open;
                    return {};
                });
            }

            [[nodiscard]] Result setProcessFunction(const ProcessFunction callback, void* user = nullptr) {
                if (calledFromDispatcher()) return std::unexpected{ ErrorType::InvalidState };
                std::scoped_lock lock(controlMutex_);

                if (state == State::Running) {
                    return std::unexpected{ ErrorType::InvalidState};
                }

                this->callback = callback;
                this->userData = user;
                return {};
            }

            // Enregistre (ou retire avec nullptr) le gestionnaire d'événements.
            [[nodiscard]] Result setEventHandler(const EventHandler handler, void* user = nullptr) {
                if (calledFromDispatcher()) return std::unexpected{ ErrorType::InvalidState };
                std::scoped_lock lock(controlMutex_);

                if (state == State::Running) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                eventHandler_ = handler;
                eventUser_ = user;
                return {};
            }

            // Compteurs et état d'échec, sans verrou ni allocation (utilisable depuis
            // n'importe quel thread, y compris pendant un start()/stop() en cours).
            [[nodiscard]] Status status() const noexcept {
                return Status{
                    .xruns = xruns_.load(std::memory_order_relaxed),
                    .failed = failed_.load(std::memory_order_acquire),
                    .realtime = static_cast<RealtimeState>(realtime_.load(std::memory_order_acquire)),
                };
            }

            [[nodiscard]] Result start() {
                if (calledFromDispatcher()) return std::unexpected{ ErrorType::InvalidState };
                std::scoped_lock lock(controlMutex_);

                if (state != State::Open) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                // Remise à zéro AVANT start_() : les événements survenus pendant le
                // démarrage ne doivent pas être effacés.
                xruns_.store(0, std::memory_order_relaxed);
                failed_.store(false, std::memory_order_release);
                realtime_.store(static_cast<std::uint8_t>(RealtimeState::Unknown), std::memory_order_release);

                return guarded([&] { return start_(); }).and_then([&]() -> Result {
                    state = State::Running;
                    startDispatcher();
                    return {};
                });
            }

            [[nodiscard]] Result stop() {
                if (calledFromDispatcher()) return std::unexpected{ ErrorType::InvalidState };
                std::scoped_lock lock(controlMutex_);

                if (state != State::Running) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return guarded([&] { return stop_(); }).and_then([&]() -> Result {
                    stopDispatcher();
                    state = State::Open;
                    return {};
                });
            }

            [[nodiscard]] Result close() {
                if (calledFromDispatcher()) return std::unexpected{ ErrorType::InvalidState };
                std::scoped_lock lock(controlMutex_);

                if (state != State::Open) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return guarded([&] { return close_(); }).and_then([&]() -> Result {
                    state = State::Closed;
                    return {};
                });
            }

            // Ne dépend pas de l'état : pas de verrou. Une exception donne une liste vide.
            [[nodiscard]] std::vector<Endpoint> getEndPoints() const noexcept {
                try {
                    return getEndPoints_();
                } catch (...) {
                    return {};
                }
            }

        protected:
            [[nodiscard]] virtual std::vector<Endpoint> getEndPoints_() const = 0;
            [[nodiscard]] virtual Result open_(EndpointConfig const &endpointCfg) = 0;
            [[nodiscard]] virtual Result start_() = 0;
            [[nodiscard]] virtual Result stop_() = 0;
            [[nodiscard]] virtual Result close_() = 0;

            // À appeler depuis n'importe quel thread, y compris le thread audio ou un
            // signal handler : uniquement des opérations atomiques sans verrou.
            void notifyXRun() noexcept {
                xruns_.fetch_add(1, std::memory_order_relaxed);
            }

            void notifyFailed() noexcept {
                failed_.store(true, std::memory_order_release);
            }

            // À appeler pendant start_() dès que le backend sait si son thread audio
            // est en temps réel. Sans appel, le statut reste Unknown.
            void notifyRealtime(const bool granted) noexcept {
                realtime_.store(static_cast<std::uint8_t>(granted ? RealtimeState::Yes : RealtimeState::No),
                                std::memory_order_release);
            }

            ProcessFunction callback = nullptr;
            void* userData = nullptr;
        private:
            enum class State { Closed, Open, Running };
            State state = State::Closed;

            // Exécute un hook de classe fille en convertissant toute exception en erreur.
            template <class Hook>
            [[nodiscard]] static Result guarded(Hook&& hook) noexcept {
                try {
                    return std::forward<Hook>(hook)();
                } catch (...) {
                    return std::unexpected{ ErrorType::ConfigurationFailed };
                }
            }

            // Vrai si l'appelant est le thread du handler d'événements de CE backend.
            [[nodiscard]] bool calledFromDispatcher() const noexcept {
                return dispatchingFor_ == this;
            }

            static inline thread_local const Backend* dispatchingFor_ = nullptr;

            std::mutex controlMutex_;

            static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
            static_assert(std::atomic<bool>::is_always_lock_free);

            static constexpr std::chrono::milliseconds kDispatchPeriod{10};

            // Le thread audio ne signale rien : il incrémente des atomiques. Ce thread
            // (non RT) les interroge et appelle le handler utilisateur, ce qui évite
            // tout verrou, futex ou appel utilisateur côté temps réel.
            void startDispatcher() {
                if (eventHandler_ == nullptr) return;

                try {
                    dispatcher_ = std::jthread([this](const std::stop_token stopToken) {
                        dispatchLoop(stopToken);
                    });
                } catch (...) {
                    // Pas d'événements, mais l'audio tourne et status() reste valable.
                }
            }

            void stopDispatcher() noexcept {
                try {
                    if (dispatcher_.joinable()) {
                        dispatcher_.request_stop();
                        dispatcher_.join();
                    }
                } catch (...) {
                    // join() ne peut échouer que sur un interblocage : on ne peut rien de plus.
                }
            }

            void dispatchLoop(const std::stop_token& stopToken) noexcept {
                dispatchingFor_ = this;   // marque ce thread : voir calledFromDispatcher()
                std::uint64_t lastXRuns = 0;
                bool failedSent = false;

                std::mutex mutex;
                std::condition_variable_any wakeup;

                while (!stopToken.stop_requested()) {
                    const std::uint64_t xruns = xruns_.load(std::memory_order_relaxed);
                    if (xruns != lastXRuns) {
                        lastXRuns = xruns;
                        eventHandler_(eventUser_, Event{EventType::XRun, xruns});
                    }

                    if (!failedSent && failed_.load(std::memory_order_acquire)) {
                        failedSent = true;
                        eventHandler_(eventUser_, Event{EventType::Failed, xruns});
                    }

                    // Se réveille au bout de kDispatchPeriod ou dès que stop est demandé.
                    std::unique_lock lock(mutex);
                    wakeup.wait_for(lock, stopToken, kDispatchPeriod, [] { return false; });
                }
            }

            std::atomic<std::uint64_t> xruns_{0};
            std::atomic<bool> failed_{false};
            std::atomic<std::uint8_t> realtime_{static_cast<std::uint8_t>(RealtimeState::Unknown)};

            EventHandler eventHandler_ = nullptr;
            void* eventUser_ = nullptr;

            // En dernier : détruit en premier, donc avant les atomiques qu'il lit.
            std::jthread dispatcher_;
    };

}
