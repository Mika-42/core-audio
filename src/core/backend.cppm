//
// Created by mika on 9/24/26.
//
module;
#include <cstdint>
#include <expected>
#include <vector>
export module mka.audio.backend;
import mka.audio.error;
import mka.audio.endpoint;
import mka.audio.process;

export namespace mka::audio {
    class Backend {
        public:
            virtual ~Backend() = default;

            virtual Result open(EndpointConfig const &endpointCfg) noexcept final {
                if (state != State::Closed) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return open_(endpointCfg).and_then([&]() -> Result {
                    state = State::Open;
                    return {};
                });
            }

            virtual Result setProcessFunction(const ProcessFunction callback) noexcept final {
                if (state == State::Running) {
                    return std::unexpected{ ErrorType::InvalidState};
                }

                this->callback = callback;
                return {};
            }

            virtual Result start() noexcept final {
                if (state != State::Open) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return start_().and_then([&]() -> Result {
                    state = State::Running;
                    return {};
                });
            }

            virtual Result stop() noexcept final {
                if (state != State::Running) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return stop_().and_then([&]() -> Result {
                   state = State::Open;
                });
            };

            virtual Result close() noexcept final {
                if (state != State::Open) {
                    return std::unexpected{ ErrorType::InvalidState };
                }
                return close_().and_then([&]() -> Result {
                    state = State::Closed;
                });
            };

            virtual std::vector<Endpoint> getEndPoints() const noexcept final {
                return getEndPoints_();
            }

        protected:
            virtual std::vector<Endpoint> getEndPoints_() const noexcept = 0;
            virtual Result open_(EndpointConfig const &endpointCfg) noexcept = 0;
            virtual Result start_() noexcept = 0;
            virtual Result stop_() noexcept = 0;
            virtual Result close_() noexcept = 0;

        private:
            enum class State { Closed, Open, Running };
            State state = State::Closed;
            ProcessFunction callback = nullptr;
    };

}
