//
// Tests de la plomberie `void* user` de Backend::setProcessFunction (correctif B1).
// Sans matériel ni serveur audio.
//
// Ne dépend que de mka.audio.backend : le test vérifie aussi que
// AudioProcessContext / ProcessFunction sont bien ré-exportés
// (`export import mka.audio.process;` dans backend.cppm).
//
#include <expected>
#include <vector>
#include <gtest/gtest.h>

import mka.audio.backend;

namespace {
    // Backend minimal : les hooks réussissent toujours, et deux accesseurs
    // exposent le callback / le contexte mémorisés par la classe de base.
    class UserDataBackend : public mka::audio::Backend {
    public:
        void* storedUserData() const noexcept { return userData; }

        void invokeCallback(const mka::audio::AudioProcessContext& ctx) const noexcept {
            if (callback) callback(userData, ctx);
        }

    protected:
        std::vector<mka::audio::Endpoint> getEndPoints_() const noexcept override { return {}; }
        mka::audio::Result open_(mka::audio::EndpointConfig const&) noexcept override { return {}; }
        mka::audio::Result start_() noexcept override { return {}; }
        mka::audio::Result stop_() noexcept override { return {}; }
        mka::audio::Result close_() noexcept override { return {}; }
    };

    struct Counter {
        int hits = 0;
    };

    void countingCallback(void* user, const mka::audio::AudioProcessContext&) noexcept {
        ++static_cast<Counter*>(user)->hits;
    }
}

TEST(BackendUserDataTest, DefaultsToNull) {
    UserDataBackend backend;
    EXPECT_EQ(backend.storedUserData(), nullptr);
}

TEST(BackendUserDataTest, SetProcessFunctionForwardsUserData) {
    UserDataBackend backend;
    Counter counter;

    ASSERT_TRUE(backend.setProcessFunction(countingCallback, &counter));
    EXPECT_EQ(backend.storedUserData(), &counter);

    backend.invokeCallback({});
    backend.invokeCallback({});
    EXPECT_EQ(counter.hits, 2);
}

TEST(BackendUserDataTest, UserDataIsOptional) {
    UserDataBackend backend;

    // Signature historique : setProcessFunction(nullptr) doit rester valide.
    ASSERT_TRUE(backend.setProcessFunction(nullptr));
    EXPECT_EQ(backend.storedUserData(), nullptr);

    backend.invokeCallback({});   // callback nul : ne doit rien appeler
}

TEST(BackendUserDataTest, NullCallbackIsNeverInvoked) {
    UserDataBackend backend;
    Counter counter;

    ASSERT_TRUE(backend.setProcessFunction(nullptr, &counter));
    backend.invokeCallback({});
    EXPECT_EQ(counter.hits, 0);
}

TEST(BackendUserDataTest, SecondCallReplacesUserData) {
    UserDataBackend backend;
    Counter a;
    Counter b;

    ASSERT_TRUE(backend.setProcessFunction(countingCallback, &a));
    ASSERT_TRUE(backend.setProcessFunction(countingCallback, &b));

    backend.invokeCallback({});
    EXPECT_EQ(a.hits, 0);
    EXPECT_EQ(b.hits, 1);
}

TEST(BackendUserDataTest, TwoBackendsKeepIndependentUserData) {
    UserDataBackend first;
    UserDataBackend second;
    Counter a;
    Counter b;

    ASSERT_TRUE(first.setProcessFunction(countingCallback, &a));
    ASSERT_TRUE(second.setProcessFunction(countingCallback, &b));

    first.invokeCallback({});
    first.invokeCallback({});
    second.invokeCallback({});

    EXPECT_EQ(a.hits, 2);
    EXPECT_EQ(b.hits, 1);
}

TEST(BackendUserDataTest, RejectedSetWhileRunningKeepsPreviousUserData) {
    UserDataBackend backend;
    Counter a;
    Counter b;

    ASSERT_TRUE(backend.setProcessFunction(countingCallback, &a));
    ASSERT_TRUE(backend.open({}));
    ASSERT_TRUE(backend.start());

    auto res = backend.setProcessFunction(countingCallback, &b);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), mka::audio::ErrorType::InvalidState);

    // Ni le callback ni le contexte ne doivent avoir changé.
    EXPECT_EQ(backend.storedUserData(), &a);
    backend.invokeCallback({});
    EXPECT_EQ(a.hits, 1);
    EXPECT_EQ(b.hits, 0);

    ASSERT_TRUE(backend.stop());
    ASSERT_TRUE(backend.close());
}
