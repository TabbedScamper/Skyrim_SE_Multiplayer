#pragma once

#include <TiltedCore/Buffer.hpp>

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
            for (float value : bone.Translation)
                aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
            for (float value : bone.Rotation)
                aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
            for (float value : bone.Scale)
                aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
        }
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader)
    {
        uint64_t count{};
        aReader.ReadBits(count, 8);
        if (count > MaxBones)
            throw std::runtime_error("evaluated pose bone count exceeds limit");
        Bones.clear();
        GraphDescriptor = 0;
        SourceTick = 0;
        if (count == 0)
            return;
        aReader.ReadBits(GraphDescriptor, 64);
        aReader.ReadBits(SourceTick, 64);
        Bones.resize(static_cast<size_t>(count));
        for (auto& bone : Bones)
        {
            for (float& value : bone.Translation)
            {
                uint64_t bits{};
                aReader.ReadBits(bits, 32);
                value = std::bit_cast<float>(static_cast<uint32_t>(bits));
            }
            for (float& value : bone.Rotation)
            {
                uint64_t bits{};
                aReader.ReadBits(bits, 32);
                value = std::bit_cast<float>(static_cast<uint32_t>(bits));
            }
            for (float& value : bone.Scale)
            {
                uint64_t bits{};
                aReader.ReadBits(bits, 32);
                value = std::bit_cast<float>(static_cast<uint32_t>(bits));
            }
        }
        if (!IsValid())
            throw std::runtime_error("invalid evaluated pose transform");
    }
};
