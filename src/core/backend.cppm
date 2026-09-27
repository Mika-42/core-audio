//
// Created by mika on 9/24/26.
//
module;
#include <expected>
#include <vector>
export module mka.audio.backend;
export import mka.audio.error;
export import mka.audio.endpoint;
export import mka.audio.constants;
import mka.audio.process;

export namespace mka::audio {
    class Backend {
        public:
            Backend() noexcept = default;
            virtual ~Backend() = default;

            Backend(const Backend&) = delete;
            Backend& operator=(const Backend&) = delete;

            Backend(Backend&&) = delete;
            Backend& operator=(Backend&&) = delete;

            [[nodiscard]] virtual Result open(EndpointConfig const &endpointCfg) final {
                if (state != State::Closed) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return open_(endpointCfg).and_then([&]() -> Result {
                    state = State::Open;
                    return {};
                });
            }

            [[nodiscard]] virtual Result setProcessFunction(const ProcessFunction callback) final {
                if (state == State::Running) {
                    return std::unexpected{ ErrorType::InvalidState};
                }

                this->callback = callback;
                return {};
            }

            [[nodiscard]] virtual Result start() final {
                if (state != State::Open) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return start_().and_then([&]() -> Result {
                    state = State::Running;
                    return {};
                });
            }

            [[nodiscard]] virtual Result stop() final {
                if (state != State::Running) {
                    return std::unexpected{ ErrorType::InvalidState };
                }

                return stop_().and_then([&]() -> Result {
                   state = State::Open;
                    return {};
                });
            };

            [[nodiscard]] virtual Result close() final {
                if (state != State::Open) {
                    return std::unexpected{ ErrorType::InvalidState };
                }
                return close_().and_then([&]() -> Result {
                    state = State::Closed;
                    return {};
                });
            };

            [[nodiscard]] virtual std::vector<Endpoint> getEndPoints() const final {
                return getEndPoints_();
            }

        protected:
            [[nodiscard]] virtual std::vector<Endpoint> getEndPoints_() const = 0;
            [[nodiscard]] virtual Result open_(EndpointConfig const &endpointCfg) = 0;
            [[nodiscard]] virtual Result start_() = 0;
            [[nodiscard]] virtual Result stop_() = 0;
            [[nodiscard]] virtual Result close_() = 0;

            ProcessFunction callback = nullptr;
        private:
            enum class State { Closed, Open, Running };
            State state = State::Closed;

    };

}
