//
// Sonde d'allocation pour les tests temps réel.
//
// Remplace operator new/delete pour compter les allocations C++ faites hors du
// thread de test pendant une fenêtre arm()/disarm().
//
// À compiler UNE SEULE FOIS par exécutable de test.
// Incompatible avec ASan/TSan/MSan (qui interceptent déjà operator new) :
// lancer ces tests dans un build sans sanitizer, ou utiliser RealtimeSanitizer.
// Les variantes over-aligned (align_val_t) ne sont pas comptées.
//
#include "rt_test_utils.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
    std::atomic<bool>        g_armed{false};
    std::atomic<std::size_t> g_count{0};
    thread_local bool        t_ignored = false;

    // Sonde "thread audio" : compte malloc/calloc/realloc/free (donc aussi les
    // allocations des libs C et les libérations) sur les seuls threads qui ont
    // exécuté le callback (marqués par markAudioThread()).
    std::atomic<bool>        g_audioArmed{false};
    std::atomic<std::size_t> g_audioCount{0};
    thread_local bool        t_audio = false;

    inline void note() noexcept {
        if (g_armed.load(std::memory_order_relaxed) && !t_ignored) {
            g_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    inline void noteAudio() noexcept {
        if (t_audio && g_audioArmed.load(std::memory_order_relaxed)) {
            g_audioCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

namespace alloc_probe {
    void ignoreCurrentThread() noexcept { t_ignored = true; }

    void arm() noexcept {
        g_count.store(0, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_release);
    }

    std::size_t disarm() noexcept {
        g_armed.store(false, std::memory_order_release);
        return g_count.load(std::memory_order_relaxed);
    }

    void markAudioThread() noexcept { t_audio = true; }

    void armAudio() noexcept {
        g_audioCount.store(0, std::memory_order_relaxed);
        g_audioArmed.store(true, std::memory_order_release);
    }

    std::size_t disarmAudio() noexcept {
        g_audioArmed.store(false, std::memory_order_release);
        return g_audioCount.load(std::memory_order_relaxed);
    }
}

// --- Interposition glibc de malloc/free ------------------------------------
// Les symboles définis dans l'exécutable remplacent ceux de la libc pour tout
// le processus (y compris libjack, libpipewire, libpulse, libasound).
#if defined(__GLIBC__)
extern "C" {
    void* __libc_malloc(std::size_t);
    void* __libc_calloc(std::size_t, std::size_t);
    void* __libc_realloc(void*, std::size_t);
    void  __libc_free(void*);

    void* malloc(const std::size_t n) { noteAudio(); return __libc_malloc(n); }
    void* calloc(const std::size_t c, const std::size_t n) { noteAudio(); return __libc_calloc(c, n); }
    void* realloc(void* p, const std::size_t n) { noteAudio(); return __libc_realloc(p, n); }
    void  free(void* p) { if (p) noteAudio(); __libc_free(p); }
}
#endif

void* operator new(const std::size_t n) {
    note();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}

void* operator new[](const std::size_t n) {
    return ::operator new(n);
}

void* operator new(const std::size_t n, const std::nothrow_t&) noexcept {
    note();
    return std::malloc(n ? n : 1);
}

void* operator new[](const std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
