//
// Created by mika on 10/2/26.
//
module;
#include <array>
export module mka.audio.backend.plateform;

export namespace mka::audio {
    enum class AudioBackend {
        Alsa, PipeWire, Jack, PulseAudio, CoreAudio, Asio, Wasapi, Wmme, DirectSound
    };

#if defined(__linux__)

    inline constexpr std::array supportedBackends = {
        AudioBackend::Alsa,
        AudioBackend::PipeWire,
        AudioBackend::Jack,
        AudioBackend::PulseAudio
    };

#elif defined(__APPLE__)

    inline constexpr std::array  supportedBackends {
        AudioBackend::CoreAudio
    };

#elif defined(_WIN32)

    inline constexpr std::array  supportedBackends {
        AudioBackend::Asio,
        AudioBackend::Wasapi,
        AudioBackend::Wmme,
        AudioBackend::DirectSound
    };

#endif
}
