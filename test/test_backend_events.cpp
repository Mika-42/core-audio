//
// Tests de B8 (status + événements) sur la classe de base, sans matériel.
//
#include <expected>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

import mka.audio.backend.abstract;

namespace {
    using namespace std::chrono_literals;

    // Backend factice : les tests jouent le rôle d'un backend réel qui détecte un xrun.
    class FakeBackend : public mka::audio::Backend {
    public:
        void fakeXRun() noexcept { notifyXRun(); }
        void fakeFailure() noexcept { notifyFailed(); }
        void fakeRealtime(bool granted) noexcept { notifyRealtime(granted); }

    protected:
        std::vector<mka::audio::Endpoint> getEndPoints_() const override { return {}; }
        mka::audio::Result open_(mka::audio::EndpointConfig const&) override { return {}; }
        mka::audio::Result start_() override { return {}; }
        mka::audio::Result stop_() override { return {}; }
        mka::audio::Result close_() override { return {}; }
    };

    struct Recorder {
        std::mutex m;
        std::vector<mka::audio::Event> events;
        std::vector<std::thread::id> threads;

        int count(const mka::audio::EventType t) {
            std::lock_guard lock(m);
            int n = 0;
            for (const auto& e : events) n += (e.type == t);
            return n;
        }
        std::uint64_t lastXRuns() {
            std::lock_guard lock(m);
            return events.empty() ? 0 : events.back().xruns;
        }
    };

    void record(void* user, const mka::audio::Event& e) noexcept {
        auto* r = static_cast<Recorder*>(user);
        std::lock_guard lock(r->m);
        r->events.push_back(e);
        r->threads.push_back(std::this_thread::get_id());
    }

    template <class Pred>
    bool waitFor(Pred p, const std::chrono::milliseconds timeout = 2000ms) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!p()) {
            if (std::chrono::steady_clock::now() > end) return false;
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    void startBackend(FakeBackend& b) {
        ASSERT_TRUE(b.open({}));
        ASSERT_TRUE(b.start());
    }
}

TEST(BackendEventsTest, StatusIsZeroInitially) {
    FakeBackend b;
    const auto s = b.status();
    EXPECT_EQ(s.xruns, 0u);
    EXPECT_FALSE(s.failed);
}

TEST(BackendEventsTest, StatusCountsWithoutAnyHandler) {
    FakeBackend b;
    startBackend(b);

    b.fakeXRun();
    b.fakeXRun();
    b.fakeXRun();
    EXPECT_EQ(b.status().xruns, 3u);
    EXPECT_FALSE(b.status().failed);

    b.fakeFailure();
    EXPECT_TRUE(b.status().failed);

    ASSERT_TRUE(b.stop());
    // Toujours lisible après stop().
    EXPECT_EQ(b.status().xruns, 3u);
    EXPECT_TRUE(b.status().failed);
}

TEST(BackendEventsTest, StatusIsResetByNextStart) {
    FakeBackend b;
    startBackend(b);
    b.fakeXRun();
    b.fakeFailure();
    ASSERT_TRUE(b.stop());

    ASSERT_TRUE(b.start());
    EXPECT_EQ(b.status().xruns, 0u);
    EXPECT_FALSE(b.status().failed);
    ASSERT_TRUE(b.stop());
}

TEST(BackendEventsTest, HandlerReceivesCumulativeXRunsOffTheCallerThread) {
    FakeBackend b;
    Recorder rec;
    ASSERT_TRUE(b.setEventHandler(record, &rec));
    startBackend(b);

    b.fakeXRun();
    ASSERT_TRUE(waitFor([&] { return rec.lastXRuns() == 1; }));

    b.fakeXRun();
    b.fakeXRun();
    ASSERT_TRUE(waitFor([&] { return rec.lastXRuns() == 3; }));

    ASSERT_TRUE(b.stop());

    std::lock_guard lock(rec.m);
    for (const auto& id : rec.threads) EXPECT_NE(id, std::this_thread::get_id());
}

TEST(BackendEventsTest, FailedIsDeliveredExactlyOnce) {
    FakeBackend b;
    Recorder rec;
    ASSERT_TRUE(b.setEventHandler(record, &rec));
    startBackend(b);

    b.fakeFailure();
    ASSERT_TRUE(waitFor([&] { return rec.count(mka::audio::EventType::Failed) == 1; }));

    b.fakeFailure();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(rec.count(mka::audio::EventType::Failed), 1);

    ASSERT_TRUE(b.stop());
}

TEST(BackendEventsTest, NoEventAfterStop) {
    FakeBackend b;
    Recorder rec;
    ASSERT_TRUE(b.setEventHandler(record, &rec));
    startBackend(b);
    ASSERT_TRUE(b.stop());

    b.fakeXRun();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(rec.count(mka::audio::EventType::XRun), 0);
}

TEST(BackendEventsTest, SetEventHandlerRejectedWhileRunning) {
    FakeBackend b;
    Recorder rec;
    startBackend(b);

    auto res = b.setEventHandler(record, &rec);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), mka::audio::ErrorType::InvalidState);

    ASSERT_TRUE(b.stop());
}

TEST(BackendEventsTest, DestroyingWhileRunningIsSafe) {
    Recorder rec;
    {
        FakeBackend b;
        ASSERT_TRUE(b.setEventHandler(record, &rec));
        startBackend(b);
        b.fakeXRun();
    }   // destructeur avec dispatcher actif : pas de crash, pas de blocage
    SUCCEED();
}

TEST(BackendRealtimeTest, UnknownUntilTheBackendReportsIt) {
    FakeBackend b;
    EXPECT_EQ(b.status().realtime, mka::audio::RealtimeState::Unknown);
    startBackend(b);
    EXPECT_EQ(b.status().realtime, mka::audio::RealtimeState::Unknown);
    ASSERT_TRUE(b.stop());
}

TEST(BackendRealtimeTest, ReflectsGrantedAndDenied) {
    FakeBackend b;
    startBackend(b);

    b.fakeRealtime(true);
    EXPECT_EQ(b.status().realtime, mka::audio::RealtimeState::Yes);
    b.fakeRealtime(false);
    EXPECT_EQ(b.status().realtime, mka::audio::RealtimeState::No);

    ASSERT_TRUE(b.stop());
}

TEST(BackendRealtimeTest, ResetToUnknownByNextStart) {
    FakeBackend b;
    startBackend(b);
    b.fakeRealtime(true);
    ASSERT_TRUE(b.stop());

    ASSERT_TRUE(b.start());
    EXPECT_EQ(b.status().realtime, mka::audio::RealtimeState::Unknown);
    ASSERT_TRUE(b.stop());
}
