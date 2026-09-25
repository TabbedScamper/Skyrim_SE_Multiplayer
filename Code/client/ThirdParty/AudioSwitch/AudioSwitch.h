#pragma once

// Auto Audio Output Switch - GPL-3.0-or-later (2026-09-13).
//
// How the game's audio moves to another device without a restart, on SE 1.5.97, AE 1.6.1170 and Skyrim 1.7.x (all
// XAudio2 2.7):
//
//  * XAudio2_7.dll is pinned (the game frees and reloads it on every rebuild) and the shared IXAudio2 vtable gets
//    CreateMasteringVoice hooked (plus Initialize and GetDeviceCount for diagnostics). When the GAME creates its
//    mastering voice, the device index is replaced by the switch target or the preferred device, and a device-loss
//    callback is registered on that engine.
//  * The game audio thread's call into its per-pass sound processing is hooked (found by its call target, so the
//    instruction offset may differ per runtime). A switch runs there, between passes: every live sound's source voice
//    is destroyed and nulled (the sounds stay), already-freed voices are cleared from the game's two voice lists, the
//    game's OWN BSXAudio2Audio shutdown and init run (init builds a new engine and mastering voice on the target), and
//    the game's own per-sound voice setup rebuilds each surviving sound. This procedure follows Live Audio Output
//    Switching SE by Maarten Harms (MIT), which found it for 1.5.97; it is ported here through Address Library IDs.
//  * A fresh engine is the only recovery XAudio2 2.7 allows after its device disappears, so an unplugged device, a
//    Windows default change, a preferred device connecting and Switch now all take the same path. Decisions are made
//    on a worker thread from Windows endpoint notifications and the engine's device-loss callback; a WASAPI render
//    probe waits until a newly connected endpoint takes audio before switching to it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace audioswitch
{
	struct Status
	{
		bool hooked{ false };
		bool managed{ false };    // the game's engine was created through the hooks
		bool attached{ false };   // a real mastering voice exists
		bool critical{ false };   // the device stopped working and no switch has succeeded since
		std::string device;
		std::uint32_t resets{ 0 };
		std::string lastResult;
	};

	// For the settings page (render thread): never blocks on the worker - returns the last snapshot while a
	// switch is running. The device list is refreshed at most once a second.
	Status GetStatus();
	std::vector<std::string> DeviceNames();
	// The lower-case Windows endpoint id the game's sound goes to right now; empty when it has no output. Any thread.
	std::string CurrentDeviceId();

	void Install();                                   // SKSEPluginLoad: pin XAudio2_7.dll and patch the vtable
	void StartWatcher();                              // SKSEPluginLoad: endpoint notifications + the reset worker
	// a_urgent skips the settle delay (the current device just stopped: switch before XAudio2 invalidates the engine);
	// a_avoidId is a device to leave out of the choice (the one that is going away).
	void RequestReset(std::string_view a_reason, bool a_force, bool a_urgent = false, std::string_view a_avoidId = {});
	bool WaitIdle(int a_timeoutMs);                   // for the DevBench tool: true once no reset is pending or running
	// For the DevBench tool: the next engine initialisation fails on purpose, leaving the game with no audio engine (a
	// rebuild on a device that cannot take audio yet) - sounds must be skipped, not crash, and a later switch recovers.
	void FailNextEngineInit();
	std::string StateJson();                          // engines, hooks, counters
	std::string DevicesJson();                        // XAudio2 device list (from a live engine) and Windows endpoints
	void LogSummary(std::string_view a_when);
}
