#pragma once

#include <array>

struct CameraStateSnapshot
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    bool operator==(const CameraStateSnapshot&) const noexcept = default;

    uint64_t Tick{};
    uint64_t AuthorityEpoch{};
    glm::vec3 Position{};
    std::array<float, 9> Rotation{};
    float Scale{1.f};
    float Fov{75.f};
    uint8_t StateId{};
};
