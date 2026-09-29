//
// Created by mika on 9/25/26.
//
module;
#include <cstdint>
export module mka.audio.process;

export namespace mka::audio {
    struct InputBuffer {
        const float* const* channels;
        std::uint32_t count;
    };

    struct OutputBuffer {
        float* const* channels;
        std::uint32_t count;
    };

    struct AudioProcessContext {
        InputBuffer input;
        OutputBuffer output;
        std::uint32_t frames;
    };

    using ProcessFunction = void (*)(void* user, const AudioProcessContext&) noexcept;
}
