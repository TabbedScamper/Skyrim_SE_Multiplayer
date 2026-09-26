#pragma once

#include <TiltedCore/Buffer.hpp>
#include <Structs/CheckedRead.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

// A bounded, lossless local-space pose transport. This does not imply that
// the receiving game's animation graph or render skeleton is safe to write.
struct EvaluatedPoseSnapshot
{
    static constexpr size_t MaxBones = 128;

    struct Bone
    {
        std::array<float, 3> Translation{};
        std::array<float, 4> Rotation{};
        std::array<float, 3> Scale{};

        bool operator==(const Bone&) const noexcept = default;
    };

    uint64_t GraphDescriptor{};
    uint64_t SourceTick{};
    std::vector<Bone> Bones{};

    bool operator==(const EvaluatedPoseSnapshot&) const noexcept = default;

    [[nodiscard]] uint64_t Checksum() const noexcept
    {
        if (Bones.empty() || !IsValid())
            return 0;
        uint64_t hash = 14695981039346656037ULL;
        auto fold = [&hash](uint64_t value)
        {
            for (unsigned i = 0; i < 8; ++i)
            {
                hash ^= static_cast<uint8_t>(value >> (i * 8));
                hash *= 1099511628211ULL;
            }
        };
        fold(GraphDescriptor);
        fold(Bones.size());
        for (const auto& bone : Bones)
        {
            for (float value : bone.Translation) fold(std::bit_cast<uint32_t>(value));
            for (float value : bone.Rotation) fold(std::bit_cast<uint32_t>(value));
            for (float value : bone.Scale) fold(std::bit_cast<uint32_t>(value));
        }
        return hash;
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        if (Bones.size() > MaxBones)
            return false;
        for (const auto& bone : Bones)
        {
            for (float value : bone.Translation)
                if (!std::isfinite(value) || std::abs(value) > 1.0e8f)
                    return false;
            for (float value : bone.Rotation)
                if (!std::isfinite(value) || std::abs(value) > 2.f)
                    return false;
            for (float value : bone.Scale)
                if (!std::isfinite(value) || std::abs(value) > 100.f)
                    return false;
        }
        return true;
    }

    // Packed per bone (~12 bytes instead of 40), so every nearby actor can stream at 20 Hz:
    //  translation: 1 flag bit; 3 x 16-bit fixed point (0.01 units, +-327.67) or 3 raw floats
    //  rotation:    smallest-three quaternion, 2-bit index + 3 x 15 bits (about 0.003 degrees)
    //  scale:       1 flag bit for (1,1,1), otherwise 3 raw floats
    static constexpr float kTranslationStep = 0.01f;
    static constexpr float kRotationRange = 0.70710678f;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        const bool valid = IsValid();
        const auto count = valid ? static_cast<uint8_t>(Bones.size()) : 0;
        aWriter.WriteBits(count, 8);
        if (count == 0)
            return;
        aWriter.WriteBits(GraphDescriptor, 64);
        aWriter.WriteBits(SourceTick, 64);
        for (const auto& bone : Bones)
        {
            const bool packed = std::abs(bone.Translation[0]) < 327.f && std::abs(bone.Translation[1]) < 327.f &&
                std::abs(bone.Translation[2]) < 327.f;
            aWriter.WriteBits(packed ? 1 : 0, 1);
            for (float value : bone.Translation)
            {
                if (packed)
                    aWriter.WriteBits(static_cast<uint16_t>(static_cast<int16_t>(std::lround(value / kTranslationStep))), 16);
                else
                    aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
            }

            // Smallest three: drop the largest component (sign-normalized to positive).
            std::array<float, 4> q = bone.Rotation;
            float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!(norm > 0.f))
            {
                q = {0.f, 0.f, 0.f, 1.f};
                norm = 1.f;
            }
            uint32_t largest = 0;
            for (uint32_t i = 1; i < 4; ++i)
                if (std::abs(q[i]) > std::abs(q[largest]))
                    largest = i;
            const float sign = q[largest] < 0.f ? -1.f : 1.f;
            aWriter.WriteBits(largest, 2);
            for (uint32_t i = 0; i < 4; ++i)
            {
                if (i == largest)
                    continue;
                const float value = std::clamp(sign * q[i] / norm / kRotationRange, -1.f, 1.f);
                aWriter.WriteBits(static_cast<uint32_t>(std::lround((value * 0.5f + 0.5f) * 32767.f)), 15);
            }

            const bool unitScale = bone.Scale[0] == 1.f && bone.Scale[1] == 1.f && bone.Scale[2] == 1.f;
            aWriter.WriteBits(unitScale ? 1 : 0, 1);
            if (!unitScale)
                for (float value : bone.Scale)
                    aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
        }
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader)
    {
        uint64_t count{};
        CheckedRead::Bits(aReader, count, 8);
        if (count > MaxBones)
            throw std::runtime_error("evaluated pose bone count exceeds limit");
        Bones.clear();
        GraphDescriptor = 0;
        SourceTick = 0;
        if (count == 0)
            return;
        CheckedRead::Bits(aReader, GraphDescriptor, 64);
        CheckedRead::Bits(aReader, SourceTick, 64);
        Bones.resize(static_cast<size_t>(count));
        for (auto& bone : Bones)
        {
            uint64_t packed{};
            CheckedRead::Bits(aReader, packed, 1);
            for (float& value : bone.Translation)
            {
                uint64_t bits{};
                if (packed)
                {
                    CheckedRead::Bits(aReader, bits, 16);
                    value = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(bits))) * kTranslationStep;
                }
                else
                {
                    CheckedRead::Bits(aReader, bits, 32);
                    value = std::bit_cast<float>(static_cast<uint32_t>(bits));
                }
            }

            uint64_t largest{};
            CheckedRead::Bits(aReader, largest, 2);
            float sum = 0.f;
            for (uint32_t i = 0; i < 4; ++i)
            {
                if (i == largest)
                    continue;
                uint64_t bits{};
                CheckedRead::Bits(aReader, bits, 15);
                const float value = (static_cast<float>(bits) / 32767.f * 2.f - 1.f) * kRotationRange;
                bone.Rotation[i] = value;
                sum += value * value;
            }
            bone.Rotation[largest] = std::sqrt((std::max)(0.f, 1.f - sum));

            uint64_t unitScale{};
            CheckedRead::Bits(aReader, unitScale, 1);
            if (unitScale)
                bone.Scale = {1.f, 1.f, 1.f};
            else
                for (float& value : bone.Scale)
                {
                    uint64_t bits{};
                    CheckedRead::Bits(aReader, bits, 32);
                    value = std::bit_cast<float>(static_cast<uint32_t>(bits));
                }
        }
        if (!IsValid())
            throw std::runtime_error("invalid evaluated pose transform");
    }
};
