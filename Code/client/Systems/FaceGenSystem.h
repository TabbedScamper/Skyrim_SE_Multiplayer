#pragma once

struct World;
struct Actor;
struct FaceGenComponent;
struct Tints;

/**
 * @brief Manages the face gen of remote players.
 */
struct FaceGenSystem
{
    static void Update(World& aWorld, Actor* apActor, FaceGenComponent& aFaceGenComponent) noexcept;
    static void Setup(World& aWorld, entt::entity aEntity, const Tints& acTints) noexcept;
    // Test only: redraw the local player's tints after each copy's tint job (on unless a control run turns it off).
    static std::atomic<bool> RestoreLocalTints;
    // After the frame's copy tint jobs (CharacterService's FaceGen loop): one redraw of the local player's tints.
    static void FlushLocalRestore() noexcept;
    // Test only (main thread): hash of the pixels of a feature-4 material's rendered tint texture, read back from the
    // GPU. 0 if unreadable.
    static uint64_t HashTintTexture(void* apMaterial) noexcept;
};
