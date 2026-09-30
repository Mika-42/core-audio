//
// Tests unitaires de mka.audio.convert (correctif B4). Aucun matériel requis.
//
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

import mka.audio.convert;

using mka::audio::convert::Layout;
using mka::audio::convert::bytesPerSample;
using mka::audio::convert::readChannel;
using mka::audio::convert::writeChannel;

namespace {
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();

    float readOne(const Layout layout, const std::vector<std::byte>& bytes) {
        float out = 0.0f;
        readChannel(layout, bytes.data(), bytesPerSample(layout), &out, 1);
        return out;
    }

    std::vector<std::byte> writeOne(const Layout layout, const float value) {
        std::vector<std::byte> bytes(bytesPerSample(layout), std::byte{0xEE});   // 0xEE : détecte un octet non écrit
        writeChannel(layout, bytes.data(), bytesPerSample(layout), &value, 1);
        return bytes;
    }

    template <class T>
    std::vector<std::byte> bytesOf(const T value) {
        std::vector<std::byte> b(sizeof(T));
        std::memcpy(b.data(), &value, sizeof(T));
        return b;
    }

    template <class T>
    T valueOf(const std::vector<std::byte>& bytes) {
        T v{};
        std::memcpy(&v, bytes.data(), sizeof(T));
        return v;
    }

    std::vector<std::byte> raw(std::initializer_list<unsigned> list) {
        std::vector<std::byte> b;
        for (const unsigned v : list) b.push_back(static_cast<std::byte>(v));
        return b;
    }
}

TEST(ConvertTest, BytesPerSample) {
    EXPECT_EQ(bytesPerSample(Layout::S16), 2u);
    EXPECT_EQ(bytesPerSample(Layout::S24Packed), 3u);
    EXPECT_EQ(bytesPerSample(Layout::S24In32), 4u);
    EXPECT_EQ(bytesPerSample(Layout::S32), 4u);
    EXPECT_EQ(bytesPerSample(Layout::F32), 4u);
    EXPECT_EQ(bytesPerSample(Layout::F64), 8u);
}

//--- S16 ---------------------------------------------------------------------

TEST(ConvertS16Test, ReadKnownValues) {
    EXPECT_FLOAT_EQ(readOne(Layout::S16, bytesOf<std::int16_t>(0)), 0.0f);
    EXPECT_FLOAT_EQ(readOne(Layout::S16, bytesOf<std::int16_t>(-32768)), -1.0f);
    EXPECT_NEAR(readOne(Layout::S16, bytesOf<std::int16_t>(32767)), 1.0f, 1.0f / 32768.0f);
    EXPECT_FLOAT_EQ(readOne(Layout::S16, bytesOf<std::int16_t>(16384)), 0.5f);
}

TEST(ConvertS16Test, WriteKnownValues) {
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, 1.0f)), 32767);
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, -1.0f)), -32767);
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, 0.0f)), 0);
    EXPECT_NEAR(valueOf<std::int16_t>(writeOne(Layout::S16, 0.5f)), 16384, 1);
}

TEST(ConvertS16Test, WriteSaturatesInsteadOfOverflowing) {
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, 2.0f)), 32767);
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, -3.5f)), -32767);
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, kInf)), 32767);
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, -kInf)), -32767);
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, 1.0e30f)), 32767);
}

TEST(ConvertS16Test, WriteNaNGivesZero) {
    EXPECT_EQ(valueOf<std::int16_t>(writeOne(Layout::S16, kNaN)), 0);
}

//--- Int24 : le bug d'origine -------------------------------------------------

TEST(ConvertS24In32Test, ReadUsesLow24Bits) {
    // Avant correctif : lu comme un int32 pleine échelle => ~48 dB trop faible.
    EXPECT_NEAR(readOne(Layout::S24In32, bytesOf<std::int32_t>(0x007FFFFF)), 1.0f, 2.0f / 8388608.0f);
    EXPECT_FLOAT_EQ(readOne(Layout::S24In32, bytesOf<std::int32_t>(0x00400000)), 0.5f);
    EXPECT_FLOAT_EQ(readOne(Layout::S24In32, bytesOf<std::uint32_t>(0x00800000u)), -1.0f);  // sign-extension
    EXPECT_FLOAT_EQ(readOne(Layout::S24In32, bytesOf<std::int32_t>(-1)), -1.0f / 8388608.0f);
}

TEST(ConvertS24In32Test, ReadIgnoresGarbageInUpperByte) {
    EXPECT_FLOAT_EQ(readOne(Layout::S24In32, bytesOf<std::uint32_t>(0x5A400000u)), 0.5f);
    EXPECT_FLOAT_EQ(readOne(Layout::S24In32, bytesOf<std::uint32_t>(0x5A800000u)), -1.0f);
}

TEST(ConvertS24In32Test, WriteStaysInside24Bits) {
    // Avant correctif : value * 2147483647 => bruit (débordement des 24 bits).
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S24In32, 1.0f)), 8388607);
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S24In32, -1.0f)), -8388607);
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S24In32, 0.0f)), 0);
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S24In32, 5.0f)), 8388607);
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S24In32, kNaN)), 0);
}

TEST(ConvertS24PackedTest, ReadLittleEndianThreeBytes) {
    EXPECT_NEAR(readOne(Layout::S24Packed, raw({0xFF, 0xFF, 0x7F})), 1.0f, 2.0f / 8388608.0f);
    EXPECT_FLOAT_EQ(readOne(Layout::S24Packed, raw({0x00, 0x00, 0x80})), -1.0f);
    EXPECT_FLOAT_EQ(readOne(Layout::S24Packed, raw({0x00, 0x00, 0x40})), 0.5f);
    EXPECT_FLOAT_EQ(readOne(Layout::S24Packed, raw({0xFF, 0xFF, 0xFF})), -1.0f / 8388608.0f);
}

TEST(ConvertS24PackedTest, WriteLittleEndianThreeBytes) {
    EXPECT_EQ(writeOne(Layout::S24Packed, 1.0f), raw({0xFF, 0xFF, 0x7F}));
    EXPECT_EQ(writeOne(Layout::S24Packed, -1.0f), raw({0x01, 0x00, 0x80}));
    EXPECT_EQ(writeOne(Layout::S24Packed, 0.0f), raw({0x00, 0x00, 0x00}));
    EXPECT_EQ(writeOne(Layout::S24Packed, 9.0f), raw({0xFF, 0xFF, 0x7F}));
}

//--- S32 ----------------------------------------------------------------------

TEST(ConvertS32Test, ReadKnownValues) {
    EXPECT_FLOAT_EQ(readOne(Layout::S32, bytesOf<std::int32_t>(std::numeric_limits<std::int32_t>::min())), -1.0f);
    EXPECT_NEAR(readOne(Layout::S32, bytesOf<std::int32_t>(std::numeric_limits<std::int32_t>::max())), 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(readOne(Layout::S32, bytesOf<std::int32_t>(0x40000000)), 0.5f);
}

TEST(ConvertS32Test, WriteDoesNotOverflowAtFullScale) {
    // Avant correctif : 1.0f * 2147483647.0f == 2^31 en float => cast hors bornes (UB).
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S32, 1.0f)), std::numeric_limits<std::int32_t>::max());
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S32, -1.0f)), -std::numeric_limits<std::int32_t>::max());
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S32, 100.0f)), std::numeric_limits<std::int32_t>::max());
    EXPECT_EQ(valueOf<std::int32_t>(writeOne(Layout::S32, kNaN)), 0);
}

//--- Flottants ----------------------------------------------------------------

TEST(ConvertFloatTest, F32IsBitExactPassThrough) {
    for (const float v : {0.0f, 0.25f, -0.7f, 1.0f, -1.0f, 1.0e-20f}) {
        EXPECT_EQ(readOne(Layout::F32, writeOne(Layout::F32, v)), v);
    }
}

TEST(ConvertFloatTest, F32KeepsHeadroomButRemovesNaN) {
    EXPECT_EQ(readOne(Layout::F32, writeOne(Layout::F32, 2.0f)), 2.0f);   // pas de saturation
    EXPECT_EQ(readOne(Layout::F32, writeOne(Layout::F32, kNaN)), 0.0f);
}

TEST(ConvertFloatTest, F64RoundTrip) {
    for (const float v : {0.0f, 0.25f, -0.7f, 3.0f}) {
        EXPECT_EQ(readOne(Layout::F64, writeOne(Layout::F64, v)), v);
    }
    EXPECT_EQ(readOne(Layout::F64, writeOne(Layout::F64, kNaN)), 0.0f);
}

//--- Aller-retour sur toute la plage, pour chaque format --------------------------

struct RoundTripCase {
    Layout layout;
    float tolerance;
};

class ConvertRoundTripTest : public ::testing::TestWithParam<RoundTripCase> {};

TEST_P(ConvertRoundTripTest, RampStaysWithinOneLsb) {
    const auto [layout, tolerance] = GetParam();
    constexpr std::size_t kFrames = 2049;

    std::vector<float> in(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        in[i] = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kFrames - 1);
    }

    std::vector<std::byte> bytes(kFrames * bytesPerSample(layout));
    writeChannel(layout, bytes.data(), bytesPerSample(layout), in.data(), kFrames);

    std::vector<float> out(kFrames, 42.0f);
    readChannel(layout, bytes.data(), bytesPerSample(layout), out.data(), kFrames);

    for (std::size_t i = 0; i < kFrames; ++i) {
        ASSERT_NEAR(out[i], in[i], tolerance) << "index " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(
    AllLayouts,
    ConvertRoundTripTest,
    ::testing::Values(
        RoundTripCase{Layout::S16, 2.0f / 32768.0f},
        RoundTripCase{Layout::S24In32, 2.0f / 8388608.0f},
        RoundTripCase{Layout::S24Packed, 2.0f / 8388608.0f},
        RoundTripCase{Layout::S32, 2.0e-7f},
        RoundTripCase{Layout::F32, 0.0f},
        RoundTripCase{Layout::F64, 0.0f}
    )
);

//--- Stride (buffers entrelacés) ----------------------------------------------------

TEST(ConvertStrideTest, ReadOneChannelOfInterleavedStereo) {
    // L R L R ... en S16 : canal R = offset 2 octets, pas 4 octets.
    const std::array<std::int16_t, 8> interleaved = {100, -100, 200, -200, 300, -300, 400, -400};
    std::array<float, 4> right{};

    readChannel(Layout::S16,
                reinterpret_cast<const std::byte*>(interleaved.data()) + sizeof(std::int16_t),
                2 * sizeof(std::int16_t), right.data(), right.size());

    for (std::size_t i = 0; i < right.size(); ++i) {
        EXPECT_FLOAT_EQ(right[i], static_cast<float>(interleaved[2 * i + 1]) / 32768.0f);
    }
}

TEST(ConvertStrideTest, WriteOneChannelLeavesTheOtherUntouched) {
    std::array<std::int16_t, 8> interleaved{};
    interleaved.fill(7);
    const std::array<float, 4> left = {1.0f, -1.0f, 0.0f, 0.5f};

    writeChannel(Layout::S16, reinterpret_cast<std::byte*>(interleaved.data()),
                 2 * sizeof(std::int16_t), left.data(), left.size());

    for (std::size_t i = 0; i < left.size(); ++i) {
        EXPECT_EQ(interleaved[2 * i + 1], 7) << "le canal droit a été écrasé";
    }
    EXPECT_EQ(interleaved[0], 32767);
    EXPECT_EQ(interleaved[2], -32767);
}

TEST(ConvertStrideTest, PackedS24InterleavedStereo) {
    // 2 canaux packed : pas de 6 octets, canal R à +3.
    std::vector<std::byte> buffer(3 * 6, std::byte{0});
    const std::array<float, 3> right = {1.0f, -1.0f, 0.0f};

    writeChannel(Layout::S24Packed, buffer.data() + 3, 6, right.data(), right.size());

    std::array<float, 3> back{};
    readChannel(Layout::S24Packed, buffer.data() + 3, 6, back.data(), back.size());
    EXPECT_NEAR(back[0], 1.0f, 2.0f / 8388608.0f);
    EXPECT_NEAR(back[1], -1.0f, 2.0f / 8388608.0f);
    EXPECT_FLOAT_EQ(back[2], 0.0f);

    for (std::size_t i = 0; i < 3; ++i) {              // canal L intact (zéro)
        for (std::size_t b = 0; b < 3; ++b) EXPECT_EQ(buffer[i * 6 + b], std::byte{0});
    }
}

TEST(ConvertStrideTest, ZeroFramesTouchesNothing) {
    std::array<float, 2> out = {9.0f, 9.0f};
    const std::array<std::int16_t, 2> in = {1, 2};
    readChannel(Layout::S16, reinterpret_cast<const std::byte*>(in.data()), 2, out.data(), 0);
    EXPECT_EQ(out[0], 9.0f);
    EXPECT_EQ(out[1], 9.0f);
}
