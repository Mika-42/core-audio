//
// Created by mika on 9/26/26.
//
#include <expected>
#include <gtest/gtest.h>

import mka.audio.backend.abstract;

namespace {
    class BackendImpl : public mka::audio::Backend {
    public:
        bool failOpen = false;
        bool failClose = false;
        bool failStart = false;
        bool failStop = false;

        BackendImpl() = default;
        ~BackendImpl() override = default;

    protected:
        std::vector<mka::audio::Endpoint>
        getEndPoints_() const noexcept override {
            return {};
        }

        mka::audio::Result open_(
            mka::audio::EndpointConfig const&
        ) noexcept override {
            if (failOpen)
                return std::unexpected(mka::audio::ErrorType::InvalidState);

            return {};
        }

        mka::audio::Result close_() noexcept override {
            if (failClose)
                return std::unexpected(mka::audio::ErrorType::InvalidState);

            return {};
        }

        mka::audio::Result start_() noexcept override {
            if (failStart)
                return std::unexpected(mka::audio::ErrorType::InvalidState);

            return {};
        }

        mka::audio::Result stop_() noexcept override {
            if (failStop)
                return std::unexpected(mka::audio::ErrorType::InvalidState);

            return {};
        }
    };
}


enum class BackendOperation {
    Open,
    Start,
    Stop,
    Close
};

struct InvalidTransition {
    std::vector<BackendOperation> setup;
    BackendOperation operation;
};

class InvalidTransitionTest
    : public ::testing::TestWithParam<InvalidTransition> {
};

mka::audio::Result execute(
    BackendImpl& backend,
    BackendOperation operation
) {
    switch (operation) {
        case BackendOperation::Open:
            return backend.open({});

        case BackendOperation::Start:
            return backend.start();

        case BackendOperation::Stop:
            return backend.stop();

        case BackendOperation::Close:
            return backend.close();
    }

    std::unreachable();
}

TEST_P(InvalidTransitionTest, RejectsTransition) {
    BackendImpl backend = {};

    for (const auto operation : GetParam().setup) {
        ASSERT_TRUE(execute(backend, operation));
    }

    auto res = execute(backend, GetParam().operation);

    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), mka::audio::ErrorType::InvalidState);
}

INSTANTIATE_TEST_SUITE_P(
    InvalidTransitions,
    InvalidTransitionTest,
    ::testing::Values(
        InvalidTransition{
            {},
            BackendOperation::Start
        },
        InvalidTransition{
            {},
            BackendOperation::Stop
        },
        InvalidTransition{
            {},
            BackendOperation::Close
        },
        InvalidTransition{
            {BackendOperation::Open},
            BackendOperation::Open
        },
        InvalidTransition{
            {BackendOperation::Open},
            BackendOperation::Stop
        },
        InvalidTransition{
            {BackendOperation::Open, BackendOperation::Start},
            BackendOperation::Open
        },
        InvalidTransition{
            {BackendOperation::Open, BackendOperation::Start},
            BackendOperation::Start
        },
        InvalidTransition{
            {BackendOperation::Open, BackendOperation::Start},
            BackendOperation::Close
        }
    )
);

//--- TEST RIGHT PIPELINE
TEST(AbstractBackendTest, TestRightPipeline) {
    BackendImpl backend;
    ASSERT_TRUE(backend.open({}));
    ASSERT_TRUE(backend.start());
    ASSERT_TRUE(backend.stop());
    ASSERT_TRUE(backend.close());
}

//--- TEST SET CALLBACK FAIL ON RUNNING
TEST(AbstractBackendTest, TestSetProcessFunctionAllowedStates) {
    BackendImpl backend;
    ASSERT_TRUE(backend.setProcessFunction(nullptr));
    ASSERT_TRUE(backend.open({}));
    ASSERT_TRUE(backend.setProcessFunction(nullptr));
    ASSERT_TRUE(backend.start());
    ASSERT_FALSE(backend.setProcessFunction(nullptr));
    ASSERT_TRUE(backend.stop());
    ASSERT_TRUE(backend.setProcessFunction(nullptr));
    ASSERT_TRUE(backend.close());
    ASSERT_TRUE(backend.setProcessFunction(nullptr));
}

//--- TEST HOOK FAILURE

TEST(AbstractBackendTest, TestOpenHookFailure) {
    BackendImpl backend;

    backend.failOpen = true;

    auto res = backend.open({});

    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), mka::audio::ErrorType::InvalidState);

    // open_() a échoué : le backend doit toujours être Closed.
    backend.failOpen = false;

    EXPECT_TRUE(backend.open({}));
}


TEST(AbstractBackendTest, TestStartHookFailure) {
    BackendImpl backend;

    ASSERT_TRUE(backend.open({}));

    backend.failStart = true;

    auto res = backend.start();

    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), mka::audio::ErrorType::InvalidState);

    // start_() a échoué : le backend doit toujours être Open.
    backend.failStart = false;

    EXPECT_TRUE(backend.start());
}


TEST(AbstractBackendTest, TestStopHookFailure) {
    BackendImpl backend;

    ASSERT_TRUE(backend.open({}));
    ASSERT_TRUE(backend.start());

    backend.failStop = true;

    auto res = backend.stop();

    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), mka::audio::ErrorType::InvalidState);

    // stop_() a échoué : le backend doit toujours être Running.
    backend.failStop = false;

    EXPECT_TRUE(backend.stop());
}


TEST(AbstractBackendTest, TestCloseHookFailure) {
    BackendImpl backend;

    ASSERT_TRUE(backend.open({}));

    backend.failClose = true;

    auto res = backend.close();

    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), mka::audio::ErrorType::InvalidState);

    // close_() a échoué : le backend doit toujours être Open.
    backend.failClose = false;

    EXPECT_TRUE(backend.close());
}