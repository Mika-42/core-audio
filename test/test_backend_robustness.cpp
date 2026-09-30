//
// Robustesse de Backend, sans matériel :
//  - une exception levée par un hook de classe fille devient une erreur, et l'état
//    du backend reste celui d'avant l'appel ;
//  - les opérations de contrôle sont sérialisées (exactement un appel concurrent
//    réussit) ;
//  - le handler d'événements ne peut pas provoquer d'interblocage en appelant stop().
//
// Pour vérifier l'absence de course de données, compiler aussi avec -fsanitize=thread.
//
#include <expected>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

import mka.audio.backend.abstract;

namespace {
    using namespace std::chrono_literals;
    using mka::audio::ErrorType;

    class ThrowingBackend : public mka::audio::Backend {
    public:
        std::atomic<bool> throwOnOpen{false};
        std::atomic<bool> throwOnStart{false};
        std::atomic<bool> throwOnStop{false};
        std::atomic<bool> throwOnClose{false};
        std::atomic<bool> throwOnEndpoints{false};

        std::atomic<int> openCalls{0};
        std::atomic<int> startCalls{0};

    protected:
        std::vector<mka::audio::Endpoint> getEndPoints_() const override {
            if (throwOnEndpoints) throw std::bad_alloc{};
            return {};
        }
        mka::audio::Result open_(mka::audio::EndpointConfig const&) override {
            ++openCalls;
            std::this_thread::sleep_for(2ms);   // élargit la fenêtre de course
            if (throwOnOpen) throw std::bad_alloc{};
            return {};
        }
        mka::audio::Result start_() override {
            ++startCalls;
            std::this_thread::sleep_for(2ms);
            if (throwOnStart) throw std::runtime_error("start");
            return {};
        }
        mka::audio::Result stop_() override {
            if (throwOnStop) throw std::runtime_error("stop");
            return {};
        }
        mka::audio::Result close_() override {
            if (throwOnClose) throw std::runtime_error("close");
            return {};
        }
    };

    // Lance `n` threads qui appellent `op` en même temps ; renvoie le nombre de succès.
    template <class Op>
    int raceCount(const int n, Op op) {
        std::atomic<bool> go{false};
        std::atomic<int> successes{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < n; ++i) {
            threads.emplace_back([&] {
                while (!go.load()) std::this_thread::yield();
                if (op()) ++successes;
            });
        }
        go = true;
        for (auto& t : threads) t.join();
        return successes.load();
    }
}

//--- Exceptions des hooks ------------------------------------------------------

TEST(BackendExceptionTest, OpenHookThrowingReturnsErrorAndStaysClosed) {
    ThrowingBackend b;
    b.throwOnOpen = true;

    auto res = b.open({});
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), ErrorType::ConfigurationFailed);

    b.throwOnOpen = false;
    EXPECT_TRUE(b.open({}));       // toujours Closed : un nouvel open est accepté
    EXPECT_TRUE(b.close());
}

TEST(BackendExceptionTest, StartHookThrowingReturnsErrorAndStaysOpen) {
    ThrowingBackend b;
    ASSERT_TRUE(b.open({}));
    b.throwOnStart = true;

    auto res = b.start();
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), ErrorType::ConfigurationFailed);

    b.throwOnStart = false;
    EXPECT_TRUE(b.start());        // toujours Open
    EXPECT_TRUE(b.stop());
    EXPECT_TRUE(b.close());
}

TEST(BackendExceptionTest, StopHookThrowingReturnsErrorAndStaysRunning) {
    ThrowingBackend b;
    ASSERT_TRUE(b.open({}));
    ASSERT_TRUE(b.start());
    b.throwOnStop = true;

    auto res = b.stop();
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), ErrorType::ConfigurationFailed);

    b.throwOnStop = false;
    EXPECT_TRUE(b.stop());         // toujours Running
    EXPECT_TRUE(b.close());
}

TEST(BackendExceptionTest, CloseHookThrowingReturnsErrorAndStaysOpen) {
    ThrowingBackend b;
    ASSERT_TRUE(b.open({}));
    b.throwOnClose = true;

    auto res = b.close();
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), ErrorType::ConfigurationFailed);

    b.throwOnClose = false;
    EXPECT_TRUE(b.close());
}

TEST(BackendExceptionTest, GetEndPointsThrowingGivesEmptyList) {
    ThrowingBackend b;
    b.throwOnEndpoints = true;
    EXPECT_TRUE(b.getEndPoints().empty());
}

//--- Concurrence -----------------------------------------------------------------

TEST(BackendThreadSafetyTest, ConcurrentOpenSucceedsExactlyOnce) {
    for (int round = 0; round < 20; ++round) {
        ThrowingBackend b;
        const int ok = raceCount(8, [&] { return b.open({}).has_value(); });
        EXPECT_EQ(ok, 1);
        EXPECT_EQ(b.openCalls.load(), 1) << "open_ appelé plusieurs fois";
        ASSERT_TRUE(b.close());
    }
}

TEST(BackendThreadSafetyTest, ConcurrentStartSucceedsExactlyOnce) {
    for (int round = 0; round < 20; ++round) {
        ThrowingBackend b;
        ASSERT_TRUE(b.open({}));
        const int ok = raceCount(8, [&] { return b.start().has_value(); });
        EXPECT_EQ(ok, 1);
        EXPECT_EQ(b.startCalls.load(), 1);
        ASSERT_TRUE(b.stop());
        ASSERT_TRUE(b.close());
    }
}

TEST(BackendThreadSafetyTest, MixedOperationsKeepAValidState) {
    ThrowingBackend b;
    std::atomic<bool> stop{false};

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            while (!stop.load()) {
                (void)b.open({});
                (void)b.start();
                (void)b.status();
                (void)b.stop();
                (void)b.close();
            }
        });
    }
    std::this_thread::sleep_for(200ms);
    stop = true;
    for (auto& t : threads) t.join();

    // Quel que soit l'entrelacement, on peut revenir à Closed puis refaire un cycle complet.
    (void)b.stop();
    (void)b.close();
    ASSERT_TRUE(b.open({}));
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.stop());
    ASSERT_TRUE(b.close());
}

//--- Ré-entrance depuis le handler ---------------------------------------------------

namespace {
    struct ReentryProbe {
        ThrowingBackend* backend = nullptr;
        std::atomic<int> stopResult{-1};    // -1 = pas encore appelé, sinon (int)ErrorType, 0 = succès
        std::atomic<int> closeResult{-1};
    };

    void reentrantHandler(void* user, const mka::audio::Event& e) noexcept {
        auto* p = static_cast<ReentryProbe*>(user);
        if (e.type != mka::audio::EventType::XRun) return;

        const auto s = p->backend->stop();      // ne doit ni bloquer ni s'attendre lui-même
        p->stopResult = s ? 0 : static_cast<int>(s.error());
        const auto c = p->backend->close();
        p->closeResult = c ? 0 : static_cast<int>(c.error());
    }

    class XRunBackend : public ThrowingBackend {
    public:
        void fakeXRun() noexcept { notifyXRun(); }
    };
}

TEST(BackendReentrancyTest, HandlerCallingStopGetsInvalidStateWithoutDeadlock) {
    XRunBackend b;
    ReentryProbe probe;
    probe.backend = &b;

    ASSERT_TRUE(b.setEventHandler(reentrantHandler, &probe));
    ASSERT_TRUE(b.open({}));
    ASSERT_TRUE(b.start());

    b.fakeXRun();

    const auto end = std::chrono::steady_clock::now() + 2s;
    while (probe.closeResult.load() == -1 && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(2ms);
    }

    EXPECT_EQ(probe.stopResult.load(), static_cast<int>(ErrorType::InvalidState));
    EXPECT_EQ(probe.closeResult.load(), static_cast<int>(ErrorType::InvalidState));

    // Le backend est intact : le thread de contrôle peut l'arrêter normalement.
    EXPECT_TRUE(b.stop());
    EXPECT_TRUE(b.close());
}
