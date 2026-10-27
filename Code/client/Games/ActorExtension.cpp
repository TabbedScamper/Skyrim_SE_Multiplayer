#include <TiltedOnlinePCH.h>

#include <Games/ActorExtension.h>

bool ActorExtension::IsRemote() const noexcept
{
    return onlineFlags.load(std::memory_order_relaxed) & kRemote;
}

bool ActorExtension::IsLocal() const noexcept
{
    return !IsRemote();
}

bool ActorExtension::IsPlayer() const noexcept
{
    return onlineFlags.load(std::memory_order_relaxed) & kPlayer;
}

bool ActorExtension::IsRemotePlayer() const noexcept
{
    return (onlineFlags.load(std::memory_order_relaxed) & (kRemote | kPlayer)) == (kRemote | kPlayer);
}

bool ActorExtension::IsLocalPlayer() const noexcept
{
    return (onlineFlags.load(std::memory_order_relaxed) & (kRemote | kPlayer)) == kPlayer;
}

void ActorExtension::SetRemote(bool aSet) noexcept
{
    if (aSet)
        onlineFlags.fetch_or(kRemote, std::memory_order_relaxed);
    else
        onlineFlags.fetch_and(~kRemote, std::memory_order_relaxed);
}

void ActorExtension::SetPlayer(bool aSet) noexcept
{
    if (aSet)
        onlineFlags.fetch_or(kPlayer, std::memory_order_relaxed);
    else
        onlineFlags.fetch_and(~kPlayer, std::memory_order_relaxed);
}
