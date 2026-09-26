#pragma once

struct Actor;

namespace LipSyncHooks
{
// The same authority rule must gate native audio and the rendered phonemes.
bool IsRemoteSpeaker(Actor* apActor) noexcept;
void Replay(Actor* apActor, const char* apFile);
}
