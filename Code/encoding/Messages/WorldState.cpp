#include <Messages/WorldState.h>
#include <cstring>

bool WorldState::Valid() const noexcept
{
    if (!Epoch || !Sequence || !Reference.BaseId || Reference.BaseId > 0xFFFFFF ||
        Reference.ModId == UINT32_MAX || !Cell.BaseId || Cell.BaseId > 0xFFFFFF ||
        Cell.ModId == UINT32_MAX || Kind >= WorldStateKind::Count || !std::isfinite(Scalar) ||
        Animation.size() > 128 || Animation.find('\0') != String::npos)
        return false;
    if (Kind != WorldStateKind::AnimationSnapshot && !AnimationData.empty()) return false;
    switch (Kind)
    {
    case WorldStateKind::Disabled:
    case WorldStateKind::Destroyed: return Value <= 1 && Scalar == 0 && Animation.empty();
    case WorldStateKind::Open: return Value <= 1 && (Scalar == 0 || Scalar == 1 || Scalar == 2 || Scalar == 3) && Animation.empty();
    case WorldStateKind::Lock: return Value <= 0x1FF && Scalar == 0 && Animation.empty();
    case WorldStateKind::DestructionHealth: return (Scalar == -1 || Scalar >= 0) &&
        (Value <= 255 || Value == UINT32_MAX) && Animation.empty();
    case WorldStateKind::FinishedSequence: return !Animation.empty() && Value == 0 && Scalar == 0;
    case WorldStateKind::AnimationEvent: return !Animation.empty() && Value == 0 && Scalar == 0;
    case WorldStateKind::AnimationSnapshot: return Animation.empty() && Value == 1 &&
        (Scalar == 0 || Scalar == 2) && WorldAnimationData::Valid(AnimationData);
    default: return false;
    }
}

void WorldState::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Epoch, 64);
    aWriter.WriteBits(Sequence, 64);
    aWriter.WriteBits(Reference.ModId, 32);
    aWriter.WriteBits(Reference.BaseId, 32);
    aWriter.WriteBits(Cell.ModId, 32);
    aWriter.WriteBits(Cell.BaseId, 32);
    aWriter.WriteBits(static_cast<uint8_t>(Kind), 8);
    aWriter.WriteBits(Value, 32);
    uint32_t bits{};
    std::memcpy(&bits, &Scalar, sizeof(bits));
    aWriter.WriteBits(bits, 32);
    aWriter.WriteBits(Animation.size(), 8);
    for (const unsigned char c : Animation)
        aWriter.WriteBits(c, 8);
    if (Kind == WorldStateKind::AnimationSnapshot)
    {
        aWriter.WriteBits(AnimationData.size(), 16);
        for (const auto byte : AnimationData) aWriter.WriteBits(byte, 8);
    }
}

bool WorldState::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t value{};
    auto read32 = [&](uint32_t& aOut) {
        if (!aReader.ReadBits(value, 32)) return false;
        aOut = static_cast<uint32_t>(value);
        return true;
    };
    if (!aReader.ReadBits(Epoch, 64) || !aReader.ReadBits(Sequence, 64) ||
        !read32(Reference.ModId) || !read32(Reference.BaseId) ||
        !read32(Cell.ModId) || !read32(Cell.BaseId) || !aReader.ReadBits(value, 8))
        return false;
    Kind = static_cast<WorldStateKind>(value);
    uint32_t bits{};
    if (!read32(Value) || !read32(bits) || !aReader.ReadBits(value, 8) || value > 128)
        return false;
    std::memcpy(&Scalar, &bits, sizeof(Scalar));
    const size_t size = static_cast<size_t>(value);
    Animation.clear();
    for (size_t i = 0; i < size; ++i)
    {
        if (!aReader.ReadBits(value, 8)) return false;
        Animation.push_back(static_cast<char>(value));
    }
    AnimationData.clear();
    if (Kind == WorldStateKind::AnimationSnapshot)
    {
        if (!aReader.ReadBits(value, 16) || value > WorldAnimationData::MaximumBytes) return false;
        const auto bytes = static_cast<size_t>(value);
        for (size_t i = 0; i < bytes; ++i)
        {
            if (!aReader.ReadBits(value, 8)) return false;
            AnimationData.push_back(static_cast<uint8_t>(value));
        }
    }
    return Valid();
}

bool WorldState::operator==(const WorldState& aOther) const noexcept
{
    return Epoch == aOther.Epoch && Sequence == aOther.Sequence && Reference == aOther.Reference &&
        Cell == aOther.Cell && Kind == aOther.Kind && Value == aOther.Value &&
        Scalar == aOther.Scalar && Animation == aOther.Animation && AnimationData == aOther.AnimationData;
}
