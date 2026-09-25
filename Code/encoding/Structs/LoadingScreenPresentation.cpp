#include <Structs/LoadingScreenPresentation.h>
#include <TiltedCore/Serialization.hpp>

void LoadingScreenPresentation::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    TiltedPhoques::Serialization::WriteVarInt(aWriter, Epoch);
    LoadScreenId.Serialize(aWriter);
}

void LoadingScreenPresentation::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    Epoch = TiltedPhoques::Serialization::ReadVarInt(aReader);
    LoadScreenId.Deserialize(aReader);
}
