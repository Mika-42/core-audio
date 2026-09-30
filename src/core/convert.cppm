//
// mka.audio.convert : conversions échantillons <-> float, sans dépendance à ALSA.
//
// Points de conception (correctif B4) :
//  - Int24 a DEUX représentations en mémoire, qu'ALSA distingue :
//      S24In32   : 24 bits utiles dans un mot de 32 bits, alignés sur les bits de
//                  poids FAIBLE (SND_PCM_FORMAT_S24, S24_LE). L'octet de poids fort
//                  est ignoré à la lecture (certains drivers y laissent n'importe quoi).
//      S24Packed : 3 octets little-endian (SND_PCM_FORMAT_S24_3LE).
//    Le code d'origine traitait S24 comme un int32 pleine échelle : signal ~48 dB trop
//    faible en lecture, débordement (bruit) en écriture.
//  - Écriture : NaN -> 0, saturation à [-1, 1], arrondi au plus proche. Plus de
//    conversion float->int hors bornes (comportement indéfini).
//  - Aucun reinterpret_cast : tous les accès passent par memcpy (pas de problème
//    d'alignement ni de strict aliasing).
//  - Le `switch` sur le format est hors de la boucle : une boucle serrée par format,
//    sans branche par échantillon, sans allocation, noexcept => utilisable en RT.
//
module;
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

export module mka.audio.convert;

namespace mka::audio::convert::detail {
    inline float sanitize(const float v) noexcept {
        if (v != v) return 0.0f;                       // NaN
        return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    }

    // Extension de signe du bit 23 (l'octet de poids fort est ignoré).
    inline std::int32_t signExtend24(const std::uint32_t u) noexcept {
        return static_cast<std::int32_t>(u << 8) >> 8;
    }

    // --- lectures ---------------------------------------------------------
    inline float loadS16(const std::byte* p) noexcept {
        std::int16_t v;
        std::memcpy(&v, p, sizeof v);
        return static_cast<float>(v) * (1.0f / 32768.0f);
    }

    inline float loadS24In32(const std::byte* p) noexcept {
        std::uint32_t u;
        std::memcpy(&u, p, sizeof u);
        return static_cast<float>(signExtend24(u)) * (1.0f / 8388608.0f);
    }

    inline float loadS24Packed(const std::byte* p) noexcept {
        const std::uint32_t u = std::to_integer<std::uint32_t>(p[0])
                              | (std::to_integer<std::uint32_t>(p[1]) << 8)
                              | (std::to_integer<std::uint32_t>(p[2]) << 16);
        return static_cast<float>(signExtend24(u)) * (1.0f / 8388608.0f);
    }

    inline float loadS32(const std::byte* p) noexcept {
        std::int32_t v;
        std::memcpy(&v, p, sizeof v);
        return static_cast<float>(static_cast<double>(v) * (1.0 / 2147483648.0));
    }

    inline float loadF32(const std::byte* p) noexcept {
        float v;
        std::memcpy(&v, p, sizeof v);
        return v;
    }

    inline float loadF64(const std::byte* p) noexcept {
        double v;
        std::memcpy(&v, p, sizeof v);
        return static_cast<float>(v);
    }

    // --- écritures --------------------------------------------------------
    inline void storeS16(std::byte* p, const float v) noexcept {
        const auto s = static_cast<std::int16_t>(std::lrintf(sanitize(v) * 32767.0f));
        std::memcpy(p, &s, sizeof s);
    }

    inline void storeS24In32(std::byte* p, const float v) noexcept {
        // Valeur signée dans [-8388607, 8388607], étendue en signe sur 32 bits.
        const auto s = static_cast<std::int32_t>(std::lrintf(sanitize(v) * 8388607.0f));
        std::memcpy(p, &s, sizeof s);
    }

    inline void storeS24Packed(std::byte* p, const float v) noexcept {
        const auto s = static_cast<std::uint32_t>(
            static_cast<std::int32_t>(std::lrintf(sanitize(v) * 8388607.0f)));
        p[0] = static_cast<std::byte>(s & 0xFFu);
        p[1] = static_cast<std::byte>((s >> 8) & 0xFFu);
        p[2] = static_cast<std::byte>((s >> 16) & 0xFFu);
    }

    inline void storeS32(std::byte* p, const float v) noexcept {
        // En double : 1.0f * 2147483647.0f s'arrondirait à 2^31 en float (débordement).
        const auto s = static_cast<std::int32_t>(
            std::lrint(static_cast<double>(sanitize(v)) * 2147483647.0));
        std::memcpy(p, &s, sizeof s);
    }

    inline void storeF32(std::byte* p, const float v) noexcept {
        const float s = (v != v) ? 0.0f : v;           // NaN -> 0, pas de saturation
        std::memcpy(p, &s, sizeof s);
    }

    inline void storeF64(std::byte* p, const float v) noexcept {
        const double s = (v != v) ? 0.0 : static_cast<double>(v);
        std::memcpy(p, &s, sizeof s);
    }

    template <class Load>
    inline void readLoop(const std::byte* src, const std::size_t stride, float* dst,
                         const std::size_t frames, Load load) noexcept {
        for (std::size_t i = 0; i < frames; ++i, src += stride) dst[i] = load(src);
    }

    template <class Store>
    inline void writeLoop(std::byte* dst, const std::size_t stride, const float* src,
                          const std::size_t frames, Store store) noexcept {
        for (std::size_t i = 0; i < frames; ++i, dst += stride) store(dst, src[i]);
    }
}

export namespace mka::audio::convert {

    enum class Layout {
        S16,        // int16, endianness native
        S24In32,    // 24 bits bas-alignés dans un int32 (SND_PCM_FORMAT_S24)
        S24Packed,  // 3 octets little-endian (SND_PCM_FORMAT_S24_3LE)
        S32,        // int32, endianness native
        F32,
        F64,
    };

    constexpr std::size_t bytesPerSample(const Layout layout) noexcept {
        switch (layout) {
            case Layout::S16:       return 2;
            case Layout::S24Packed: return 3;
            case Layout::S24In32:
            case Layout::S32:
            case Layout::F32:       return 4;
            case Layout::F64:       return 8;
        }
        return 0;
    }

    // Lit `frames` échantillons espacés de `strideBytes` octets (un canal d'un buffer
    // entrelacé ou non) vers `dst` en float.
    inline void readChannel(const Layout layout, const std::byte* src, const std::size_t strideBytes,
                            float* dst, const std::size_t frames) noexcept {
        using namespace detail;
        switch (layout) {
            case Layout::S16:       readLoop(src, strideBytes, dst, frames, loadS16); return;
            case Layout::S24In32:   readLoop(src, strideBytes, dst, frames, loadS24In32); return;
            case Layout::S24Packed: readLoop(src, strideBytes, dst, frames, loadS24Packed); return;
            case Layout::S32:       readLoop(src, strideBytes, dst, frames, loadS32); return;
            case Layout::F32:       readLoop(src, strideBytes, dst, frames, loadF32); return;
            case Layout::F64:       readLoop(src, strideBytes, dst, frames, loadF64); return;
        }
    }

    // Écrit `frames` échantillons float dans `dst` (espacés de `strideBytes` octets).
    inline void writeChannel(const Layout layout, std::byte* dst, const std::size_t strideBytes,
                             const float* src, const std::size_t frames) noexcept {
        using namespace detail;
        switch (layout) {
            case Layout::S16:       writeLoop(dst, strideBytes, src, frames, storeS16); return;
            case Layout::S24In32:   writeLoop(dst, strideBytes, src, frames, storeS24In32); return;
            case Layout::S24Packed: writeLoop(dst, strideBytes, src, frames, storeS24Packed); return;
            case Layout::S32:       writeLoop(dst, strideBytes, src, frames, storeS32); return;
            case Layout::F32:       writeLoop(dst, strideBytes, src, frames, storeF32); return;
            case Layout::F64:       writeLoop(dst, strideBytes, src, frames, storeF64); return;
        }
    }
}
