#pragma once

#include <TiltedCore/Buffer.hpp>
#include <Structs/CheckedRead.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

// Owner-authored render-skeleton locals, indexed by the active graph's
// boneNodes array. Presence is explicit because some graph entries have no
// live NiNode. Never treat this as an owned native pointer or a save format.
struct VisualBoneSnapshot
{
    static constexpr size_t MaxBones = 128;

    struct Bone
    {
        bool Present{};
        std::array<float, 9> Rotation{};
        std::array<float, 3> Translation{};
        float Scale{1.f};

        bool operator==(const Bone&) const noexcept = default;
    };

    uint64_t GraphDescriptor{};
    uint64_t SourceTick{};
    Bone RootWorld{};
    std::vector<Bone> Bones{};

    bool operator==(const VisualBoneSnapshot&) const noexcept = default;

    [[nodiscard]] bool IsValid() const noexcept
    {
        if (Bones.size() > MaxBones)
            return false;
        const auto validBone = [](const Bone& bone) noexcept
        {
            if (!bone.Present)
                return true;
            for (const float value : bone.Rotation)
                if (!std::isfinite(value) || std::abs(value) > 2.f)
                    return false;
            for (const float value : bone.Translation)
                if (!std::isfinite(value) || std::abs(value) > 1.0e8f)
                    return false;
            if (!std::isfinite(bone.Scale) || bone.Scale < 0.f ||
                bone.Scale > 100.f)
                return false;
            return true;
        };
        if (!validBone(RootWorld))
            return false;
        for (const auto& bone : Bones)
        {
            if (!validBone(bone))
                return false;
        }
        return true;
    }

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
        auto foldBone = [&fold](const Bone& bone)
        {
            fold(bone.Present ? 1 : 0);
            if (!bone.Present)
                return;
            for (float value : bone.Rotation)
                fold(std::bit_cast<uint32_t>(value));
            for (float value : bone.Translation)
                fold(std::bit_cast<uint32_t>(value));
            fold(std::bit_cast<uint32_t>(bone.Scale));
        };
        foldBone(RootWorld);
        for (const auto& bone : Bones)
            foldBone(bone);
        return hash;
    }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
    {
        const auto count = IsValid() ? static_cast<uint8_t>(Bones.size()) : 0;
        aWriter.WriteBits(count, 8);
        if (!count)
            return;
        aWriter.WriteBits(GraphDescriptor, 64);
        aWriter.WriteBits(SourceTick, 64);
        const auto writeBone = [&aWriter](const Bone& bone) noexcept
        {
            aWriter.WriteBits(bone.Present ? 1 : 0, 1);
            if (!bone.Present)
                return;
            for (float value : bone.Rotation)
                aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
            for (float value : bone.Translation)
                aWriter.WriteBits(std::bit_cast<uint32_t>(value), 32);
            aWriter.WriteBits(std::bit_cast<uint32_t>(bone.Scale), 32);
        };
        writeBone(RootWorld);
        for (const auto& bone : Bones)
            writeBone(bone);
    }

    void Deserialize(TiltedPhoques::Buffer::Reader& aReader)
    {
        uint64_t count{};
        CheckedRead::Bits(aReader, count, 8);
        if (count > MaxBones)
            throw std::runtime_error("visual bone count exceeds limit");
        GraphDescriptor = 0;
        SourceTick = 0;
        Bones.clear();
        RootWorld = {};
        if (!count)
            return;
        CheckedRead::Bits(aReader, GraphDescriptor, 64);
        CheckedRead::Bits(aReader, SourceTick, 64);
        const auto readBone = [&aReader](Bone& bone)
        {
            uint64_t present{};
            CheckedRead::Bits(aReader, present, 1);
            bone.Present = present != 0;
            if (!bone.Present)
                return;
            for (float& value : bone.Rotation)
            {
                uint64_t bits{};
                CheckedRead::Bits(aReader, bits, 32);
                value = std::bit_cast<float>(static_cast<uint32_t>(bits));
            }
            for (float& value : bone.Translation)
            {
                uint64_t bits{};
                CheckedRead::Bits(aReader, bits, 32);
                value = std::bit_cast<float>(static_cast<uint32_t>(bits));
            }
            uint64_t bits{};
            CheckedRead::Bits(aReader, bits, 32);
            bone.Scale = std::bit_cast<float>(static_cast<uint32_t>(bits));
        };
        readBone(RootWorld);
        Bones.resize(static_cast<size_t>(count));
        for (auto& bone : Bones)
            readBone(bone);
        if (!IsValid())
            throw std::runtime_error("invalid visual bone transform");
    }
};
