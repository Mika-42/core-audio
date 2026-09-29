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

    inline void note() noexcept {
        if (g_armed.load(std::memory_order_relaxed) && !t_ignored) {
            g_count.fetch_add(1, std::memory_order_relaxed);
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
}

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
