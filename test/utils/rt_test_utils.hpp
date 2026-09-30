#pragma once
//
// Utilitaires communs aux tests "contrat temps réel" (test_rt_*.cpp).
//
// Aucune dépendance aux modules mka.audio : les types de contexte sont des
// paramètres de template, ce qui permet d'inclure ce header avant les `import`.
//
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

// --- Sonde d'allocation (implémentée dans alloc_probe.cpp) ---------------------
//
// Compte les appels à `operator new` faits par tous les threads SAUF ceux
// déclarés "ignorés" (le thread de test). Comme les libs audio (ALSA, JACK,
// PipeWire, PulseAudio) sont en C, toute allocation C++ comptée ici vient du
// code de mka.audio ou du callback utilisateur.
namespace alloc_probe {
    void ignoreCurrentThread() noexcept;   // à appeler depuis le thread de test
    void arm() noexcept;                   // remet le compteur à 0 et démarre le comptage
    std::size_t disarm() noexcept;         // arrête le comptage, renvoie le total

    // Sonde malloc/calloc/realloc/free (glibc) limitée aux threads audio :
    // le callback de test appelle markAudioThread() à chaque cycle.
    void markAudioThread() noexcept;
    void armAudio() noexcept;
    std::size_t disarmAudio() noexcept;
}

namespace rt_test {

    // Valeur "sale" écrite dans les sorties : -60 dBFS en continu, donc
    // inoffensive si le flux arrive vraiment à une enceinte.
    inline constexpr float kDirtyValue = 1.0e-3f;

    struct ContractState {
        std::atomic<int>   calls{0};
        std::atomic<bool>  outputDirtyOnEntry{false};
        std::atomic<bool>  sawOutput{false};
        std::atomic<float> inputSink{0.0f};
    };

    // Contrat "sortie à zéro" : à l'entrée du callback, tous les échantillons de
    // sortie doivent valoir exactement 0. On salit ensuite tout le buffer : un
    // backend qui réutilise un buffer sans le remettre à zéro est détecté au
    // cycle suivant.
    template <class Ctx>
    void dirtyOutput(ContractState& s, const Ctx& ctx) noexcept {
        if (ctx.frames == 0) return;

        for (std::uint32_t c = 0; c < ctx.output.count; ++c) {
            float* out = ctx.output.channels[c];
            for (std::uint32_t i = 0; i < ctx.frames; ++i) {
                if (out[i] != 0.0f) {
                    s.outputDirtyOnEntry.store(true, std::memory_order_relaxed);
                }
                out[i] = kDirtyValue;
            }
            s.sawOutput.store(true, std::memory_order_relaxed);
        }
        s.calls.fetch_add(1, std::memory_order_relaxed);
    }

    // Lit chaque échantillon d'entrée (détecte un pointeur ou une taille invalide).
    template <class Ctx>
    void touchInput(ContractState& s, const Ctx& ctx) noexcept {
        if (ctx.frames == 0) return;

        float acc = 0.0f;
        for (std::uint32_t c = 0; c < ctx.input.count; ++c) {
            const float* in = ctx.input.channels[c];
            for (std::uint32_t i = 0; i < ctx.frames; ++i) acc += in[i];
        }
        s.inputSink.store(acc, std::memory_order_relaxed);
        s.calls.fetch_add(1, std::memory_order_relaxed);
    }

    template <class Pred>
    bool waitFor(Pred&& pred,
                 const std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    // Nombre de cycles à observer avant de conclure (assez pour que les pools
    // de buffers tournants, ex. PipeWire, réutilisent des buffers déjà salis).
    inline constexpr int kMinCycles = 64;

    // --- Contrat "aucun callback après stop()" ----------------------------------

    struct StopContractState {
        std::atomic<int> calls{0};
    };

    // Callback de test : marque le thread comme audio (sonde malloc) et compte.
    template <class Ctx>
    void countAudioCall(StopContractState& s, const Ctx&) noexcept {
        alloc_probe::markAudioThread();
        s.calls.fetch_add(1, std::memory_order_relaxed);
    }

    // Démarre/arrête `rounds` fois en martelant status() depuis un autre thread.
    // Après chaque stop(), le compteur d'appels ne doit plus bouger.
    // Renvoie une chaîne vide si tout va bien, sinon la raison de l'échec.
    template <class B>
    const char* stopStress(B& backend, StopContractState& s, const int rounds = 10) {
        std::atomic<bool> hammering{true};
        std::thread hammer([&] {
            while (hammering.load(std::memory_order_relaxed)) {
                (void)backend.status();
                std::this_thread::yield();
            }
        });

        const char* failure = "";
        for (int r = 0; r < rounds && *failure == '\0'; ++r) {
            const int before = s.calls.load();
            if (!backend.start()) { failure = "start() a échoué"; break; }
            if (!waitFor([&] { return s.calls.load() >= before + 4; })) failure = "pas de cycles après start()";
            if (!backend.stop()) { failure = "stop() a échoué"; break; }

            const int atStop = s.calls.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (s.calls.load() != atStop) failure = "callback appelé après le retour de stop()";
        }

        hammering.store(false, std::memory_order_relaxed);
        hammer.join();
        return failure;
    }

}
