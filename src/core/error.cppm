//
// Created by mika on 9/25/26.
//
module;
#include <expected>
export module mka.audio.error;

export namespace mka::audio {
    enum class ErrorType {
        InvalidState,
        EndpointUnavailable,
        FormatNotSupported,
        ChannelsNotSupported,
        SampleRateNotSupported,
        BufferSizeNotSupported,
        ConfigurationFailed,
    };
    using Result = std::expected<void, ErrorType>;
}
