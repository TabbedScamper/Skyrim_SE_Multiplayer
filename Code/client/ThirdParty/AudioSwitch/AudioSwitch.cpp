// Auto Audio Output Switch - GPL-3.0-or-later (2026-09-13).
//
// The device switch rebuilds the game's own audio engine on the game's audio thread. That procedure follows
// Live Audio Output Switching SE by Maarten Harms (MIT; its notice is in THIRD_PARTY_NOTICES.md), which found it
// for Skyrim SE 1.5.97. This file ports it to SE 1.5.97, AE 1.6.1170 and Skyrim 1.7.x through Address Library IDs
// (each function and global confirmed in the 1.5.97 and 1.7.104 code), and adds the preferred device, the settings
// page and the DevBench tool. See AudioSwitch.h.
#include "PCH.h"

#include "AudioSwitch.h"

#include "Settings.h"
#include "XAudio27.h"
#include "utils/Logger.h"

#include <Windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <tlhelp32.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audioswitch
{
	namespace
	{
		using namespace xa27;

		// ---- game layout (identical on 1.5.97 and 1.7.104 in the disassembly) ----
		constexpr std::uintptr_t kAudioMasterOff = 0x58;   // BSXAudio2Audio: IXAudio2MasteringVoice*
		constexpr std::uintptr_t kSoundVoiceOff = 0x128;   // BSXAudio2GameSound: its source voice
		constexpr std::uintptr_t kMgrMapCapacity = 0x34;   // BSAudioManager sound table: slot count
		constexpr std::uintptr_t kMgrMapEntries = 0x50;    // BSAudioManager sound table: entry array
		constexpr std::uintptr_t kEntryStride = 0x18;      // {key, sound* @+8, next @+0x10}; empty when next == 0
		constexpr std::uintptr_t kVoiceListCount = 0x88;   // global voice list: count; data inline at +8 when flags < 0
		constexpr int kSlotInit = 1;                       // BSXAudio2Audio vtable
		constexpr int kSlotShutdown = 2;
		constexpr std::uintptr_t kAudioEngineOff = 0x50;   // BSXAudio2Audio: IXAudio2* (the game's shutdown nulls it)

		struct GameAddresses
		{
			std::uintptr_t threadLoop{ 0 };     // BSAudioManagerThread run loop
			std::uintptr_t processSounds{ 0 };  // the per-pass sound processing it calls
			std::uintptr_t setupSound{ 0 };     // create + set up one sound's source voice (rcx = sound) -> bool
			std::uintptr_t buildVoice{ 0 };     // builds the source voice through the engine; called once, from setupSound
			std::uintptr_t createCall{ 0 };     // the game's `call [rip+disp]` to CoCreateInstance(CLSID_XAudio2) in engine creation
			std::uintptr_t voiceListA{ 0 };
			std::uintptr_t voiceListB{ 0 };
			std::uintptr_t audioObject{ 0 };    // global BSXAudio2Audio*
			std::uintptr_t audioVtable{ 0 };
		};
		GameAddresses g_addr;

		std::string Narrow(const wchar_t* a_text)
		{
			if (!a_text || !*a_text) { return {}; }
			const int len = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
			if (len <= 1) { return {}; }
			std::string out(static_cast<std::size_t>(len - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), len, nullptr, nullptr);
			return out;
		}

		std::wstring Widen(const std::string& a_text)
		{
			if (a_text.empty()) { return {}; }
			const int len = MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), -1, nullptr, 0);
			std::wstring out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), L'\0');
			if (len > 1) { MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), -1, out.data(), len); }
			return out;
		}

		std::string Lower(std::string a_text)
		{
			for (char& c : a_text) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
			return a_text;
		}

		std::string Trim(const std::string& a_text)
		{
			const auto b = a_text.find_first_not_of(" \t\r\n\"");
			if (b == std::string::npos) { return {}; }
			const auto e = a_text.find_last_not_of(" \t\r\n\"");
			return a_text.substr(b, e - b + 1);
		}

		std::string Hr(HRESULT a_hr) { return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr)); }

		std::string Ptr(const void* a_ptr) { return std::format("{:X}", reinterpret_cast<std::uintptr_t>(a_ptr)); }

		std::string EscapeJson(std::string_view a_in)
		{
			std::string out;
			out.reserve(a_in.size() + 8);
			for (const char c : a_in)
			{
				switch (c)
				{
				case '\\': out += "\\\\"; break;
				case '"': out += "\\\""; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20) { out += std::format("\\u{:04x}", static_cast<unsigned char>(c)); }
					else { out += c; }
					break;
				}
			}
			return out;
		}

		const char* EndpointStateName(DWORD a_state)
		{
			switch (a_state)
			{
			case DEVICE_STATE_ACTIVE: return "active";
			case DEVICE_STATE_DISABLED: return "disabled";
			case DEVICE_STATE_NOTPRESENT: return "not present";
			case DEVICE_STATE_UNPLUGGED: return "unplugged";
			default: return "unknown";
			}
		}

		// ---- SEH-guarded raw access (no C++ objects with destructors in these functions) ----
		struct SehInfo
		{
			DWORD code{ 0 };
			void* at{ nullptr };
			ULONG_PTR accessKind{ 0 };    // 0 read, 1 write, 8 execute (access violations)
			ULONG_PTR accessTarget{ 0 };
			DWORD64 frames[24]{};
			int frameCount{ 0 };
		};

		// Runs as the exception FILTER, while the faulting frames are still on the stack (after the handler runs they are
		// unwound and overwritten). No allocation, no logging: the fault may have happened with a lock held.
		int FillSeh(EXCEPTION_POINTERS* a_ep, SehInfo* a_out)
		{
			if (a_ep && a_ep->ExceptionRecord)
			{
				a_out->code = a_ep->ExceptionRecord->ExceptionCode;
				a_out->at = a_ep->ExceptionRecord->ExceptionAddress;
				if (a_ep->ExceptionRecord->NumberParameters >= 2)
				{
					a_out->accessKind = a_ep->ExceptionRecord->ExceptionInformation[0];
					a_out->accessTarget = a_ep->ExceptionRecord->ExceptionInformation[1];
				}
			}
			if (a_ep && a_ep->ContextRecord)
			{
				CONTEXT ctx = *a_ep->ContextRecord;
				__try
				{
					for (int i = 0; i < 24 && ctx.Rip; ++i)
					{
						a_out->frames[a_out->frameCount++] = ctx.Rip;
						DWORD64 imageBase = 0;
						PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
						if (!fn)
						{
							ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
							ctx.Rsp += 8;
							continue;
						}
						PVOID handlerData = nullptr;
						DWORD64 establisher = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			return EXCEPTION_EXECUTE_HANDLER;
		}

		// defined further down, used by the switch
		std::string FrameText(DWORD64 a_pc);
		void LogFault(const char* a_where, const SehInfo& a_seh);

		void* ReadPtr(void* a_base, std::uintptr_t a_off)
		{
			__try { return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(a_base) + a_off); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		}

		void WritePtr(void* a_base, std::uintptr_t a_off, void* a_value)
		{
			__try { *reinterpret_cast<void**>(static_cast<std::uint8_t*>(a_base) + a_off) = a_value; }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		int CallGameSlot(void* a_object, int a_slot, SehInfo* a_seh)
		{
			__try
			{
				auto fn = reinterpret_cast<int (*)(void*)>((*reinterpret_cast<void***>(a_object))[a_slot]);
				return fn(a_object);
			}
			__except (FillSeh(GetExceptionInformation(), a_seh)) { return -1; }
		}

		bool CallSetupSound(std::uintptr_t a_fn, void* a_sound)
		{
			__try { return reinterpret_cast<bool (*)(void*)>(a_fn)(a_sound); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void DestroyVoiceSafe(void* a_voice)
		{
			__try { static_cast<IXAudio2Voice*>(a_voice)->DestroyVoice(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		// The live game sounds, read from BSAudioManager's sound table. a_out receives up to a_max sound pointers.
		int SnapshotSounds(void* a_mgr, void** a_out, int a_max)
		{
			int n = 0;
			__try
			{
				const std::uint32_t capacity = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(a_mgr) + kMgrMapCapacity);
				auto* entries = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(a_mgr) + kMgrMapEntries);
				if (!entries || capacity == 0 || capacity > 0x10000) { return 0; }
				for (std::uint32_t i = 0; i < capacity && n < a_max; ++i)
				{
					const std::uint8_t* e = entries + static_cast<std::size_t>(i) * kEntryStride;
					if (*reinterpret_cast<void* const*>(e + 0x10) != nullptr)
					{
						if (void* s = *reinterpret_cast<void* const*>(e + 0x08)) { a_out[n++] = s; }
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
			return n;
		}

		// Removes sounds from the table: every sound when a_onlyVoiceless is false, otherwise only sounds that have no
		// source voice (so the game's per-sound update never dereferences a missing voice). Returns how many.
		int RemoveSounds(void* a_mgr, bool a_onlyVoiceless)
		{
			int removed = 0;
			__try
			{
				const std::uint32_t capacity = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(a_mgr) + kMgrMapCapacity);
				auto* entries = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(a_mgr) + kMgrMapEntries);
				if (!entries || capacity == 0 || capacity > 0x10000) { return 0; }
				for (std::uint32_t i = 0; i < capacity; ++i)
				{
					std::uint8_t* e = entries + static_cast<std::size_t>(i) * kEntryStride;
					if (*reinterpret_cast<void**>(e + 0x10) == nullptr) { continue; }
					void* s = *reinterpret_cast<void**>(e + 0x08);
					const bool voiceless = !s || *reinterpret_cast<void**>(static_cast<std::uint8_t*>(s) + kSoundVoiceOff) == nullptr;
					if (!a_onlyVoiceless || voiceless)
					{
						*reinterpret_cast<void**>(e + 0x08) = nullptr;
						*reinterpret_cast<void**>(e + 0x10) = nullptr;
						++removed;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return removed;
		}

		// A voice whose private implementation has no vtable was already freed; the game's shutdown would crash on it.
		bool VoiceIsFreed(void* a_voice)
		{
			__try
			{
				void* impl = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(a_voice) + 0x10);
				return !impl || *reinterpret_cast<void**>(impl) == nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return true; }
		}

		// After the game's own audio shutdown: the output-mixer table (list A: {effect object, submix voice} slots the game's
		// sound outputs keep by index) still points at the voices and effect objects that shutdown destroyed, and the spare
		// pool (list B) would hand them out again. Every A slot is emptied (the game null-checks a slot and fills empty ones
		// when it makes a new mixer) and B is emptied. Returns the slots emptied, or -1 on a fault.
		int ClearOutputMixers(std::uintptr_t a_table, std::uintptr_t a_pool, std::uint32_t* a_poolWas)
		{
			int cleared = 0;
			*a_poolWas = 0;
			__try
			{
				auto* head = reinterpret_cast<std::uint8_t*>(a_table);
				const std::int32_t flags = *reinterpret_cast<std::int32_t*>(head);
				const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(head + kVoiceListCount);
				auto* data = flags < 0 ? head + 8 : *reinterpret_cast<std::uint8_t**>(head + 8);
				if (data && count <= 0x4000)
				{
					for (std::uint32_t i = 0; i < count; ++i)
					{
						std::uint8_t* entry = data + static_cast<std::size_t>(i) * 0x10;
						if (*reinterpret_cast<void**>(entry) || *reinterpret_cast<void**>(entry + 8))
						{
							*reinterpret_cast<void**>(entry) = nullptr;
							*reinterpret_cast<void**>(entry + 8) = nullptr;
							++cleared;
						}
					}
				}
				auto* pool = reinterpret_cast<std::uint8_t*>(a_pool);
				*a_poolWas = *reinterpret_cast<std::uint32_t*>(pool + kVoiceListCount);
				*reinterpret_cast<std::uint32_t*>(pool + kVoiceListCount) = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
			return cleared;
		}

		// Nulls entries of a global voice list whose voice was already freed, so the game's shutdown skips them.
		int ScrubVoiceList(std::uintptr_t a_head)
		{
			int scrubbed = 0;
			__try
			{
				auto* head = reinterpret_cast<std::uint8_t*>(a_head);
				const std::int32_t flags = *reinterpret_cast<std::int32_t*>(head);
				const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(head + kVoiceListCount);
				if (count == 0 || count > 0x4000) { return 0; }
				auto* data = flags < 0 ? head + 8 : *reinterpret_cast<std::uint8_t**>(head + 8);
				if (!data) { return 0; }
				for (std::uint32_t i = 0; i < count; ++i)
				{
					std::uint8_t* entry = data + static_cast<std::size_t>(i) * 0x10;
					void* voice = *reinterpret_cast<void**>(entry + 8);
					if (voice && VoiceIsFreed(voice))
					{
						*reinterpret_cast<void**>(entry) = nullptr;
						*reinterpret_cast<void**>(entry + 8) = nullptr;
						++scrubbed;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return scrubbed;
		}

		// ---- state ----
		struct State
		{
			void* audio{ nullptr };             // BSXAudio2Audio
			IXAudio2* engine{ nullptr };
			IXAudio2Voice* master{ nullptr };
			std::string deviceId;               // lower-case endpoint id
			std::string deviceName;
			UINT32 channels{ 0 };
			UINT32 rate{ 0 };
			std::uint32_t switches{ 0 };
			std::uint32_t failures{ 0 };
			std::string lastResult{ "waiting for the game's audio engine" };
		};

		std::mutex g_stateLock;
		State g_state;
		std::atomic<bool> g_critical{ false };
		std::atomic<std::uint32_t> g_criticalCount{ 0 };
		std::atomic<std::uint64_t> g_passes{ 0 };

		// ---- audio thread watch: where the game's audio thread is when a switch is not taken (the first real
		// headset unplug, 2026-09-14, left the switch untaken for 10 s with no critical error) ----
		std::atomic<std::uint32_t> g_audioTid{ 0 };
		std::atomic<std::int64_t> g_lastEnterMs{ 0 };   // the audio thread entered the sound processing
		std::atomic<std::int64_t> g_lastExitMs{ 0 };    // ... and left it
		std::atomic<std::uint64_t> g_iterations{ 0 };
		std::atomic<std::int64_t> g_lastPassMs{ 0 };    // the engine's last OnProcessingPassEnd
		std::atomic<int> g_swapStep{ 0 };               // 0 idle, 1 detach, 2 scrub, 3 shutdown, 4 init, 5 revive

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		thread_local int t_inside = 0;

		struct Inside
		{
			Inside() { ++t_inside; }
			~Inside() { --t_inside; }
		};

		CreateMasteringFn g_origMaster{ nullptr };
		CreateSourceFn g_origCreateSource{ nullptr };
		CreateSubmixFn g_origCreateSubmix{ nullptr };

		// Send lists with an empty output. XAudio2 2.7's CreateSendList reads pOutputVoice+0xE8 with no null check
		// (crash XAudio2_7+0x28B53, seen in game 2026-09-14 nine seconds after a real headset unplug; Live Audio Output
		// Switching SE documents the same address). The game emits one when an effect mixer it routes a sound through
		// is gone after a rebuild; the send is dropped and the sound keeps its other outputs.
		std::atomic<std::uint32_t> g_droppedSends{ 0 };
		std::atomic<bool> g_dropLogged{ false };
		using SetOutputVoicesFn = HRESULT(STDMETHODCALLTYPE*)(IXAudio2Voice*, const VoiceSends*);
		struct VoiceVtableHook
		{
			void** vtable{ nullptr };
			SetOutputVoicesFn original{ nullptr };
		};
		VoiceVtableHook g_voiceVtables[6]{};
		std::mutex g_voiceVtableLock;
		InitializeFn g_origInitialize{ nullptr };
		StartEngineFn g_origStartEngine{ nullptr };
		StopEngineFn g_origStopEngine{ nullptr };
		GetDeviceCountFn g_origDeviceCount{ nullptr };
		using ProcessSoundsFn = void (*)(void*);
		ProcessSoundsFn g_origProcessSounds{ nullptr };
		std::atomic<bool> g_hooked{ false };
		std::atomic<bool> g_threadHooked{ false };
		std::atomic<std::uint32_t> g_initializeCalls{ 0 };
		std::atomic<std::uint32_t> g_deviceCountCalls{ 0 };
		std::string g_installResult{ "not run" };
		std::string g_threadHookResult{ "not run" };
		std::string g_voiceGuardResult{ "not run" };
		std::atomic<std::uint32_t> g_nullEngineSkips{ 0 };
		std::atomic<std::int64_t> g_lastNullEngineResetMs{ 0 };
		std::atomic<bool> g_failNextInit{ false };  // DevBench op=failinit: reproduce a rebuild that leaves no engine

		// Engines a rebuild makes the game release, kept alive by our own reference until a rebuild succeeds. Another mod
		// may have cached the engine pointer - Better AltTab stores the IXAudio2* from the game's CoCreateInstance call and
		// calls StopEngine/StartEngine on it at every focus change (Nexus report, falsification episode 49: a rebuild whose
		// init did not produce a new engine left it calling through freed memory). A held engine is stopped and dead but a
		// live COM object, so such a call returns an error instead of crashing. Audio thread only (the switch runs there).
		std::vector<IUnknown*> g_heldEngines;
		std::vector<IUnknown*> g_pinnedEngines;  // held for good: a mod's hook was repaired, so its cached pointer never moves on
		// Taken by the switch from the engine-creation check to the end of the game's init, and by an engine call forwarded from
		// another thread, so a forwarded StopEngine/StartEngine never lands on an engine that is being torn down or built.
		std::recursive_mutex g_pinLock;
		// What another mod last asked of the engine from outside the audio thread: -1 nothing, 0 stopped, 1 started. Better
		// AltTab stops the engine when the game loses focus and starts it when it comes back; a switch in between would
		// otherwise leave the new engine playing while the game is in the background.
		std::atomic<int> g_otherModEngineState{ -1 };
		std::atomic<bool> g_inRebuild{ false };
		std::atomic<std::uint32_t> g_heldCount{ 0 };

		bool AddRefSafe(IUnknown* a_engine)
		{
			__try
			{
				a_engine->AddRef();
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void ReleaseSafe(IUnknown* a_engine)
		{
			__try { a_engine->Release(); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		void HoldEngine(IUnknown* a_engine, const char* a_why)
		{
			if (!a_engine) { return; }
			for (IUnknown* e : g_heldEngines) { if (e == a_engine) { return; } }
			for (IUnknown* e : g_pinnedEngines) { if (e == a_engine) { return; } }
			if (!AddRefSafe(a_engine)) { return; }
			g_heldEngines.push_back(a_engine);
			g_heldCount = static_cast<std::uint32_t>(g_heldEngines.size());
			logger::info("holding engine {} ({}) so a pointer another mod cached stays valid; {} held", static_cast<void*>(a_engine), a_why, g_heldEngines.size());
		}

		void ReleaseHeldEngines(IUnknown* a_keep)
		{
			int released = 0;
			for (IUnknown* e : g_heldEngines)
			{
				bool pinned = false;
				for (IUnknown* q : g_pinnedEngines) { if (q == e) { pinned = true; } }
				if (e == a_keep || pinned) { continue; }
				ReleaseSafe(e);
				++released;
			}
			g_heldEngines.clear();
			g_heldCount = static_cast<std::uint32_t>(g_pinnedEngines.size());
			if (released) { logger::info("released {} held engine(s) after the rebuild succeeded", released); }
		}

		// ---- the game's engine-creation call, checked before every rebuild ----
		// Better AltTab (Nexus 121342) write_call<6>-hooks this exact call (SE 66746+0x44, AE/1.7 67952+0xB7) to cache the new
		// engine, and its hook's branch slot lives in a trampoline block that is gone by the time the game has loaded its data
		// (its log shows a fresh "Default Trampoline => 0B / 64B" then). The first engine creation works; every later one - every
		// AAOS rebuild - jumps through unmapped memory (1.0.2 fault log: SkyrimSE.exe+BFCBB4 reading 0x7FF6C17D0000; falsification
		// episode 50). Before a rebuild the call is decoded; if it goes through memory that is not there any more it is pointed
		// back at the game's own CoCreateInstance import. The mod whose hook is dropped keeps the engine it cached, which is then
		// pinned (kept referenced for good) so its StopEngine/StartEngine calls stay harmless.
		std::string g_createCallState{ "not checked" };

		std::uintptr_t FindImportSlot(const char* a_dll, const char* a_func)
		{
			const std::uintptr_t base = REL::Module::get().base();
			__try
			{
				auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
				auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
				const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				if (!dir.VirtualAddress) { return 0; }
				for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
				{
					if (_stricmp(reinterpret_cast<const char*>(base + d->Name), a_dll) != 0 || !d->OriginalFirstThunk) { continue; }
					auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
					auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
					for (; names->u1.AddressOfData; ++names, ++iat)
					{
						if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) { continue; }
						auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
						if (std::strcmp(reinterpret_cast<const char*>(byName->Name), a_func) == 0) { return reinterpret_cast<std::uintptr_t>(iat); }
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return 0;
		}

		// MEM_COMMIT, not PAGE_NOACCESS/PAGE_GUARD, and executable when asked; the whole range inside one region.
		bool MemoryUsable(std::uintptr_t a_addr, std::size_t a_len, bool a_exec, DWORD* a_state, DWORD* a_protect)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			*a_state = 0;
			*a_protect = 0;
			if (!VirtualQuery(reinterpret_cast<LPCVOID>(a_addr), &mbi, sizeof(mbi))) { return false; }
			*a_state = mbi.State;
			*a_protect = mbi.Protect;
			if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & 0xFF) == PAGE_NOACCESS) { return false; }
			if (a_exec)
			{
				const DWORD p = mbi.Protect & 0xFF;
				if (p != PAGE_EXECUTE && p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) { return false; }
			}
			return a_addr + a_len <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}

		// form: 6 = FF 15 disp32, 5 = E8 rel32, 0 = something else; -1 when reading faulted. POD only (SEH).
		int DecodeCall(std::uintptr_t a_site, std::uintptr_t* a_slot, std::uintptr_t* a_target)
		{
			*a_slot = 0;
			*a_target = 0;
			__try
			{
				const auto* p = reinterpret_cast<const std::uint8_t*>(a_site);
				std::int32_t rel = 0;
				if (p[0] == 0xFF && p[1] == 0x15)
				{
					std::memcpy(&rel, p + 2, sizeof(rel));
					*a_slot = a_site + 6 + static_cast<std::intptr_t>(rel);
					return 6;
				}
				if (p[0] == 0xE8)
				{
					std::memcpy(&rel, p + 1, sizeof(rel));
					*a_target = a_site + 5 + static_cast<std::intptr_t>(rel);
					return 5;
				}
				return 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		std::uintptr_t ReadSlot(std::uintptr_t a_slot)
		{
			__try { return *reinterpret_cast<std::uintptr_t*>(a_slot); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

		// Returns true when the call was repaired in this check.
		bool CheckEngineCreateCall()
		{
			const std::uintptr_t site = g_addr.createCall;
			if (!site) { return false; }
			static std::uintptr_t iat = 0;
			if (!iat) { iat = FindImportSlot("ole32.dll", "CoCreateInstance"); }

			std::uintptr_t slot = 0;
			std::uintptr_t target = 0;
			const int form = DecodeCall(site, &slot, &target);
			DWORD state = 0;
			DWORD protect = 0;
			std::string broken;
			if (form == 6)
			{
				if (iat && slot == iat)
				{
					g_createCallState = "the game's own CoCreateInstance import";
					return false;
				}
				if (!MemoryUsable(slot, sizeof(std::uintptr_t), false, &state, &protect))
				{
					broken = std::format("its branch slot {:X} is not usable memory (state 0x{:X}, protect 0x{:X})", slot, state, protect);
				}
				else
				{
					target = ReadSlot(slot);
					if (!MemoryUsable(target, 1, true, &state, &protect))
					{
						broken = std::format("its target {:X} (through slot {:X}) is not executable memory (state 0x{:X}, protect 0x{:X})", target, slot, state, protect);
					}
				}
			}
			else if (form == 5)
			{
				if (!MemoryUsable(target, 1, true, &state, &protect))
				{
					broken = std::format("its target {:X} is not executable memory (state 0x{:X}, protect 0x{:X})", target, state, protect);
				}
			}
			else
			{
				g_createCallState = form < 0 ? "unreadable" : "unexpected instruction";
				return false;
			}

			if (broken.empty())
			{
				const std::string now = std::format("hooked by another mod, working ({})", FrameText(form == 6 ? ReadSlot(slot) : target));
				if (now != g_createCallState) { logger::info("the game's engine-creation call is {}", now); }
				g_createCallState = now;
				return false;
			}
			if (!iat)
			{
				g_createCallState = "broken, not repairable (the game's CoCreateInstance import was not found)";
				logger::error("the game's engine-creation call at {} is broken - {} - and the game's CoCreateInstance import was not found to repair it", FrameText(site), broken);
				return false;
			}
			const std::intptr_t disp = static_cast<std::intptr_t>(iat) - static_cast<std::intptr_t>(site + 6);
			if (disp < INT32_MIN || disp > INT32_MAX) { return false; }
			std::uint8_t bytes[6]{ 0xFF, 0x15, 0, 0, 0, 0 };
			const auto disp32 = static_cast<std::int32_t>(disp);
			std::memcpy(bytes + 2, &disp32, sizeof(disp32));
			REL::safe_write(site, bytes, sizeof(bytes));
			g_createCallState = "repaired (another mod's hook had been left pointing at freed memory)";
			logger::warn("the game's engine-creation call at {} went through memory that no longer exists - {}. Another mod's hook left it dangling "
						 "(Better AltTab's XAudio2 hook does this once the game's data has loaded). It now calls the game's own CoCreateInstance import again, "
						 "so the audio engine can be rebuilt; that mod's hook no longer runs there.",
				FrameText(site), broken);
			return true;
		}

		void PinEngine(IUnknown* a_engine)
		{
			std::scoped_lock l(g_pinLock);
			if (!a_engine) { return; }
			for (IUnknown* e : g_pinnedEngines) { if (e == a_engine) { return; } }
			g_pinnedEngines.push_back(a_engine);
			logger::info("keeping engine {} referenced for good: the mod whose hook was repaired still holds its pointer", static_cast<void*>(a_engine));
		}

		// swap handoff: the worker decides and waits; the audio thread performs
		std::mutex g_swapLock;
		std::condition_variable g_swapCv;
		std::atomic<bool> g_swapPending{ false };
		bool g_swapDone{ false };
		std::string g_swapTargetId;  // lower-case endpoint id the new mastering voice must use (guarded by g_swapLock)

		void SetResult(std::string a_text, bool a_failure = false)
		{
			std::scoped_lock l(g_stateLock);
			g_state.lastResult = std::move(a_text);
			if (a_failure) { ++g_state.failures; }
		}

		std::string LastResult()
		{
			std::scoped_lock l(g_stateLock);
			return g_state.lastResult;
		}

		// ---- XAudio2 device list (only called with an engine the caller knows is alive) ----
		struct Device
		{
			UINT32 index{ 0 };
			std::string id;  // lower-case
			std::string name;
			UINT32 role{ 0 };
		};

		std::vector<Device> ListXaDevices(IXAudio2* a_engine)
		{
			std::vector<Device> out;
			UINT32 count = 0;
			HRESULT hr;
			{
				Inside inside;
				hr = a_engine->GetDeviceCount(&count);
			}
			if (FAILED(hr)) { return out; }
			for (UINT32 i = 0; i < count; ++i)
			{
				DeviceDetails details{};
				if (FAILED(a_engine->GetDeviceDetails(i, &details))) { continue; }
				details.DeviceID[255] = L'\0';
				details.DisplayName[255] = L'\0';
				out.push_back({ i, Lower(Narrow(details.DeviceID)), Narrow(details.DisplayName), details.Role });
			}
			return out;
		}

		// ---- Windows endpoints (safe whether or not the engine is alive) ----
		struct Endpoint
		{
			std::string id;  // lower-case
			std::string name;
			DWORD state{ 0 };
			bool isDefault{ false };
		};

		std::vector<Endpoint> ListEndpoints(bool a_activeOnly)
		{
			std::vector<Endpoint> out;
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			IMMDeviceEnumerator* enumerator = nullptr;
			if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator)
			{
				std::string defaultId;
				IMMDevice* def = nullptr;
				if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def)
				{
					LPWSTR id = nullptr;
					if (SUCCEEDED(def->GetId(&id)) && id)
					{
						defaultId = Lower(Narrow(id));
						CoTaskMemFree(id);
					}
					def->Release();
				}
				IMMDeviceCollection* collection = nullptr;
				const DWORD mask = a_activeOnly ? DEVICE_STATE_ACTIVE : (DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED | DEVICE_STATE_DISABLED);
				if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, mask, &collection)) && collection)
				{
					UINT count = 0;
					collection->GetCount(&count);
					for (UINT i = 0; i < count; ++i)
					{
						IMMDevice* device = nullptr;
						if (FAILED(collection->Item(i, &device)) || !device) { continue; }
						Endpoint ep;
						LPWSTR rawId = nullptr;
						if (SUCCEEDED(device->GetId(&rawId)) && rawId)
						{
							ep.id = Lower(Narrow(rawId));
							CoTaskMemFree(rawId);
						}
						device->GetState(&ep.state);
						IPropertyStore* store = nullptr;
						if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store)
						{
							PROPVARIANT value;
							PropVariantInit(&value);
							if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) { ep.name = Narrow(value.pwszVal); }
							PropVariantClear(&value);
							store->Release();
						}
						device->Release();
						ep.isDefault = !defaultId.empty() && ep.id == defaultId;
						out.push_back(std::move(ep));
					}
					collection->Release();
				}
				enumerator->Release();
			}
			if (SUCCEEDED(co)) { CoUninitialize(); }
			return out;
		}

		// sPreferredDevice: a full endpoint id, or part of a device name (case-insensitive). Empty = none.
		const Endpoint* PreferredEndpoint(const std::vector<Endpoint>& a_list)
		{
			const std::string pref = Lower(Trim(settings::GetPreferredDevice()));
			if (pref.empty()) { return nullptr; }
			for (const auto& e : a_list) { if (e.id == pref) { return &e; } }
			for (const auto& e : a_list) { if (Lower(e.name).find(pref) != std::string::npos) { return &e; } }
			return nullptr;
		}

		// A newly connected endpoint (Bluetooth especially) can report ACTIVE before its stream takes buffers; an
		// XAudio2 2.7 mastering voice created then crashes the mixer. A shared-mode WASAPI render probe proves it is
		// ready and warms it up. (Technique from Live Audio Output Switching SE, MIT.)
		bool EndpointReady(const std::string& a_id)
		{
			if (a_id.empty()) { return true; }
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			bool ok = false;
			IMMDeviceEnumerator* enumerator = nullptr;
			if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator)
			{
				IMMDevice* device = nullptr;
				const std::wstring wid = Widen(a_id);
				if (SUCCEEDED(enumerator->GetDevice(wid.c_str(), &device)) && device)
				{
					DWORD state = 0;
					if (SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE)
					{
						IAudioClient* client = nullptr;
						if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && client)
						{
							WAVEFORMATEX* format = nullptr;
							if (SUCCEEDED(client->GetMixFormat(&format)) && format)
							{
								if (SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr)))
								{
									IAudioRenderClient* render = nullptr;
									if (SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))) && render)
									{
										BYTE* buffer = nullptr;
										if (SUCCEEDED(render->GetBuffer(64, &buffer)) && buffer)
										{
											render->ReleaseBuffer(0, 0);
											ok = true;
										}
										render->Release();
									}
								}
								CoTaskMemFree(format);
							}
							client->Release();
						}
					}
					device->Release();
				}
				enumerator->Release();
			}
			else
			{
				ok = true;  // cannot probe: do not block the switch
			}
			if (SUCCEEDED(co)) { CoUninitialize(); }
			return ok;
		}

		// ---- engine callback: an unplugged device kills the engine; turn that into a rebuild ----
		class EngineCallback final : public IXAudio2EngineCallback
		{
		public:
			void STDMETHODCALLTYPE OnProcessingPassStart() override {}
			void STDMETHODCALLTYPE OnProcessingPassEnd() override
			{
				g_passes.fetch_add(1, std::memory_order_relaxed);
				g_lastPassMs.store(NowMs(), std::memory_order_relaxed);
			}
			void STDMETHODCALLTYPE OnCriticalError(HRESULT a_error) override
			{
				logger::warn("the audio engine reported a critical error {} (engine passes {})", Hr(a_error), g_passes.load());
				g_criticalCount.fetch_add(1);
				g_critical = true;
				RequestReset("the audio engine lost its device", false, true);
			}
		};
		EngineCallback g_callback;

		bool IsGameAudioObject(void* a_candidate)
		{
			return a_candidate && g_addr.audioVtable && ReadPtr(a_candidate, 0) == reinterpret_cast<void*>(g_addr.audioVtable);
		}

		void* GameAudioObject()
		{
			if (!g_addr.audioObject) { return nullptr; }
			void* object = ReadPtr(reinterpret_cast<void*>(g_addr.audioObject), 0);
			return IsGameAudioObject(object) ? object : nullptr;
		}

		// ---- send filtering ----
		constexpr UINT32 kMaxSends = 64;

		// Returns the list to hand to XAudio2: the caller's own, or a_tmp holding it without its empty outputs.
		const VoiceSends* FilterSends(const VoiceSends* a_sends, VoiceSends* a_tmp, SendDescriptor* a_buf, UINT32* a_dropped)
		{
			*a_dropped = 0;
			if (!a_sends) { return a_sends; }
			__try
			{
				const UINT32 count = a_sends->SendCount;
				if (count == 0 || count > kMaxSends || !a_sends->pSends) { return a_sends; }
				UINT32 kept = 0;
				for (UINT32 i = 0; i < count; ++i)
				{
					if (a_sends->pSends[i].pOutputVoice) { a_buf[kept++] = a_sends->pSends[i]; }
				}
				if (kept == count) { return a_sends; }
				*a_dropped = count - kept;
				a_tmp->SendCount = kept;
				a_tmp->pSends = kept ? a_buf : nullptr;
				return a_tmp;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return a_sends; }
		}

		void NoteDropped(UINT32 a_dropped, const char* a_where)
		{
			if (!a_dropped) { return; }
			const auto total = g_droppedSends.fetch_add(a_dropped) + a_dropped;
			if (!g_dropLogged.exchange(true))
			{
				logger::warn("{}: dropped {} send(s) with no output voice (an effect mixer the game routes through is gone after the rebuild); {} dropped so far", a_where, a_dropped, total);
			}
		}

		SetOutputVoicesFn OriginalSetOutputVoices(IXAudio2Voice* a_voice)
		{
			void** vtable = *reinterpret_cast<void***>(a_voice);
			for (const auto& h : g_voiceVtables) { if (h.vtable == vtable) { return h.original; } }
			return nullptr;
		}

		HRESULT STDMETHODCALLTYPE HookSetOutputVoices(IXAudio2Voice* a_this, const VoiceSends* a_sends)
		{
			const SetOutputVoicesFn original = OriginalSetOutputVoices(a_this);
			if (!original) { return E_FAIL; }  // unreachable: an entry is filled before its slot is patched
			VoiceSends tmp{};
			SendDescriptor buf[kMaxSends];
			UINT32 dropped = 0;
			const VoiceSends* pass = FilterSends(a_sends, &tmp, buf, &dropped);
			NoteDropped(dropped, "IXAudio2Voice::SetOutputVoices");
			return original(a_this, pass);
		}

		// Each voice class (source, submix, mastering, ...) has its own vtable in XAudio2_7.dll; its SetOutputVoices slot is
		// patched the first time a voice of that class is created.
		void HookVoiceVtable(IXAudio2Voice* a_voice)
		{
			if (!a_voice) { return; }
			void** vtable = *reinterpret_cast<void***>(a_voice);
			std::scoped_lock l(g_voiceVtableLock);
			for (const auto& h : g_voiceVtables) { if (h.vtable == vtable) { return; } }
			for (auto& h : g_voiceVtables)
			{
				if (h.vtable) { continue; }
				void** entry = vtable + 1;
				DWORD old = 0;
				if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old))
				{
					logger::warn("could not unprotect IXAudio2Voice::SetOutputVoices on vtable {} (error {})", Ptr(vtable), GetLastError());
					return;
				}
				h.original = reinterpret_cast<SetOutputVoicesFn>(*entry);
				h.vtable = vtable;
				*entry = reinterpret_cast<void*>(&HookSetOutputVoices);
				VirtualProtect(entry, sizeof(void*), old, &old);
				logger::info("hooked IXAudio2Voice::SetOutputVoices (voice vtable {}, original {})", Ptr(vtable), Ptr(h.original));
				return;
			}
			logger::warn("a seventh voice vtable {} was seen; its sends are not filtered", Ptr(vtable));
		}

		// ---- hooks ----
		HRESULT STDMETHODCALLTYPE HookCreateSource(IXAudio2* a_this, IXAudio2Voice** a_out, const WAVEFORMATEX* a_format, UINT32 a_flags, float a_maxRatio, void* a_callback, const VoiceSends* a_sends, void* a_chain)
		{
			VoiceSends tmp{};
			SendDescriptor buf[kMaxSends];
			UINT32 dropped = 0;
			const VoiceSends* pass = FilterSends(a_sends, &tmp, buf, &dropped);
			NoteDropped(dropped, "IXAudio2::CreateSourceVoice");
			const HRESULT hr = g_origCreateSource(a_this, a_out, a_format, a_flags, a_maxRatio, a_callback, pass, a_chain);
			if (SUCCEEDED(hr) && a_out) { HookVoiceVtable(*a_out); }
			return hr;
		}

		HRESULT STDMETHODCALLTYPE HookCreateSubmix(IXAudio2* a_this, IXAudio2Voice** a_out, UINT32 a_channels, UINT32 a_rate, UINT32 a_flags, UINT32 a_stage, const VoiceSends* a_sends, void* a_chain)
		{
			VoiceSends tmp{};
			SendDescriptor buf[kMaxSends];
			UINT32 dropped = 0;
			const VoiceSends* pass = FilterSends(a_sends, &tmp, buf, &dropped);
			NoteDropped(dropped, "IXAudio2::CreateSubmixVoice");
			const HRESULT hr = g_origCreateSubmix(a_this, a_out, a_channels, a_rate, a_flags, a_stage, pass, a_chain);
			if (SUCCEEDED(hr) && a_out) { HookVoiceVtable(*a_out); }
			return hr;
		}

		HRESULT STDMETHODCALLTYPE HookInitialize(IXAudio2* a_this, UINT32 a_flags, UINT32 a_processor)
		{
			HRESULT hr = g_origInitialize(a_this, a_flags, a_processor);
			if (t_inside == 0 && SUCCEEDED(hr) && g_failNextInit.exchange(false))
			{
				hr = static_cast<HRESULT>(0x88960004);  // XAUDIO2_E_DEVICE_INVALID: the game releases the engine and leaves +0x50 null
				logger::warn("DevBench tool: this engine initialisation is failed on purpose, so the game is left with no audio engine");
			}
			if (t_inside == 0 && FAILED(hr) && g_inRebuild.load())
			{
				HoldEngine(a_this, "its Initialize failed inside a rebuild");
			}
			if (t_inside == 0)
			{
				const auto n = g_initializeCalls.fetch_add(1) + 1;
				logger::info("engine {} initialised (flags=0x{:X}): {} - call {}", Ptr(a_this), a_flags, Hr(hr), n);
			}
			return hr;
		}

		// ---- XAudio2 2.7's own deadlock on device removal ----
		// When the device an engine plays on disappears, two XAudio2 threads can both call LEAPCORE::CLeapSystem::OnCriticalError:
		// the audio session's disconnect callback, and the engine's graph (processing) thread, which found the device gone itself.
		// OnCriticalError takes the CLeapSystem critical section (this+0x88) and, when the caller is not the graph thread, calls
		// StopGraph while holding it - StopGraph sets the graph thread's stop event and waits, without a timeout, for that thread to
		// exit. The graph thread's call is the last thing it does before exiting, and it blocks on the same critical section: a
		// deadlock inside XAudio2. The game's audio thread then blocks in IXAudio2SourceVoice::GetState and the game stops
		// responding. Found from a Task Manager dump with Microsoft's public symbols (falsification episode 51); the unplug hangs
		// first blamed on this mod's DestroyVoice (episodes 46 and 48) were this same race.
		// The guard (XAudio2_7 9.29.1962, June 2010 - the build Skyrim installs - verified before patching): on the graph thread,
		// OnCriticalError takes the critical section with TryEnterCriticalSection and calls the original while holding it (the
		// section is recursive). If another thread holds it and has already recorded the error (this+0xB8), that thread is stopping
		// the graph and waiting for this one, so the call returns and the thread exits; the error is still reported by the other
		// thread. XAudio2's own logic is unchanged when the two paths do not collide.
		constexpr DWORD kXa27TimeStamp = 0x4C0643CC;
		constexpr DWORD kXa27ImageSize = 0x8B000;
		constexpr std::uintptr_t kXa27SlotRva = 0x224E8;       // ILeapCallback-side vtable slot holding CLeapSystem::OnCriticalError
		constexpr std::uintptr_t kXa27OnCriticalErrorRva = 0x2EB80;
		constexpr std::uintptr_t kLeapCritSec = 0x88;          // CCriticalSectionLock at +0x80, its CRITICAL_SECTION at +8
		constexpr std::uintptr_t kLeapError = 0xB8;            // first critical error (lock cmpxchg)
		constexpr std::uintptr_t kLeapGraph = 0xC8;            // graph manager; its thread id at +0x18

		using LeapOnCriticalErrorFn = void (*)(void*, HRESULT);
		LeapOnCriticalErrorFn g_origLeapOnCriticalError{ nullptr };
		std::atomic<std::uint32_t> g_leapCollisions{ 0 };
		std::string g_leapGuardResult{ "not installed" };

		DWORD LeapGraphThread(void* a_system)
		{
			__try
			{
				void* graph = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(a_system) + kLeapGraph);
				return graph ? *reinterpret_cast<DWORD*>(static_cast<std::uint8_t*>(graph) + 0x18) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

		void LeapOnCriticalErrorHook(void* a_system, HRESULT a_error)
		{
			const DWORD graphTid = a_system ? LeapGraphThread(a_system) : 0;
			if (graphTid != 0 && GetCurrentThreadId() == graphTid)
			{
				auto* cs = reinterpret_cast<CRITICAL_SECTION*>(static_cast<std::uint8_t*>(a_system) + kLeapCritSec);
				const auto* recorded = reinterpret_cast<const LONG*>(static_cast<std::uint8_t*>(a_system) + kLeapError);
				for (int i = 0; i < 2000; ++i)
				{
					if (TryEnterCriticalSection(cs))
					{
						g_origLeapOnCriticalError(a_system, a_error);
						LeaveCriticalSection(cs);
						return;
					}
					if (*recorded != 0)
					{
						const auto n = g_leapCollisions.fetch_add(1) + 1;
						logger::warn("XAudio2 deadlock avoided: its graph thread reported the lost device (0x{:08X}) while another XAudio2 thread "
									 "was already handling it (0x{:08X}) and waiting for the graph thread to stop; the graph thread now exits instead of "
									 "waiting for that thread ({} so far)",
							static_cast<std::uint32_t>(a_error), static_cast<std::uint32_t>(*recorded), n);
						return;
					}
					Sleep(1);
				}
			}
			g_origLeapOnCriticalError(a_system, a_error);
		}

		bool BytesMatch(std::uintptr_t a_addr, std::initializer_list<int> a_bytes)
		{
			__try
			{
				const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
				std::size_t i = 0;
				for (const int b : a_bytes)
				{
					if (b >= 0 && p[i] != static_cast<std::uint8_t>(b)) { return false; }
					++i;
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void InstallLeapGuard(HMODULE a_module)
		{
			const auto base = reinterpret_cast<std::uintptr_t>(a_module);
			const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->FileHeader.TimeDateStamp != kXa27TimeStamp || nt->OptionalHeader.SizeOfImage != kXa27ImageSize)
			{
				g_leapGuardResult = std::format("not installed: XAudio2_7.dll is not the June 2010 build it was written for (timestamp 0x{:08X}, image size 0x{:X})",
					nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
				logger::warn("XAudio2 deadlock guard {}", g_leapGuardResult);
				return;
			}
			const std::uintptr_t fn = base + kXa27OnCriticalErrorRva;
			const bool bytes =
				BytesMatch(fn, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x80, 0x00, 0x00, 0x00 }) &&
				BytesMatch(fn + 0x29, { 0xF0, 0x0F, 0xB1, 0xAF, 0xB8, 0x00, 0x00, 0x00 }) &&           // lock cmpxchg [rdi+0B8h],ebp
				BytesMatch(fn + 0x3B, { 0x48, 0x8B, 0x87, 0xC8, 0x00, 0x00, 0x00, 0x8B, 0x58, 0x18 });  // mov rax,[rdi+0C8h]; mov ebx,[rax+18h]
			void** slot = reinterpret_cast<void**>(base + kXa27SlotRva);
			if (!bytes || static_cast<void*>(*slot) != reinterpret_cast<void*>(fn))
			{
				g_leapGuardResult = "not installed: CLeapSystem::OnCriticalError or its vtable slot did not match the expected code";
				logger::warn("XAudio2 deadlock guard {}", g_leapGuardResult);
				return;
			}
			DWORD old = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
			{
				g_leapGuardResult = std::format("not installed: the vtable slot could not be made writable (error {})", GetLastError());
				logger::warn("XAudio2 deadlock guard {}", g_leapGuardResult);
				return;
			}
			g_origLeapOnCriticalError = reinterpret_cast<LeapOnCriticalErrorFn>(*slot);
			*slot = reinterpret_cast<void*>(&LeapOnCriticalErrorHook);
			VirtualProtect(slot, sizeof(void*), old, &old);
			g_leapGuardResult = "installed (XAudio2_7 9.29.1962)";
			logger::info("XAudio2 deadlock guard installed: CLeapSystem::OnCriticalError on the graph thread no longer waits for a thread that waits for it");
		}

		// ---- StartEngine / StopEngine from other mods ----
		// Called on an engine AAOS keeps alive for another mod (pinned: that mod's hook was repaired, so its cached pointer
		// never moves on), the call is passed to the engine that is playing, so the mod keeps working - Better AltTab's
		// "stop the audio in the background" goes on muting the game after a switch. The game's own calls come from the audio
		// thread (its shutdown stops the old engine) and pass straight through.
		bool IsPinnedEngine(IXAudio2* a_engine)
		{
			std::scoped_lock l(g_pinLock);
			for (IUnknown* e : g_pinnedEngines) { if (static_cast<void*>(e) == static_cast<void*>(a_engine)) { return true; } }
			return false;
		}

		IXAudio2* PlayingEngineOtherThan(IXAudio2* a_engine)
		{
			if (g_inRebuild.load()) { return nullptr; }
			std::scoped_lock l(g_stateLock);
			return (g_state.engine && g_state.engine != a_engine) ? g_state.engine : nullptr;
		}

		void LogForwarded(bool a_start, IXAudio2* a_from, IXAudio2* a_to)
		{
			static std::atomic<int> last{ -1 };
			const int now = a_start ? 1 : 0;
			if (last.exchange(now) != now)
			{
				logger::info("another mod called {} on kept engine {}; passed to the playing engine {}", a_start ? "StartEngine" : "StopEngine", Ptr(a_from), Ptr(a_to));
			}
		}

		bool FromOtherThread() { return t_inside == 0 && GetCurrentThreadId() != g_audioTid.load(); }

		HRESULT STDMETHODCALLTYPE HookStartEngine(IXAudio2* a_this)
		{
			if (FromOtherThread())
			{
				g_otherModEngineState = 1;
				if (IsPinnedEngine(a_this))
				{
					std::scoped_lock l(g_pinLock);
					if (IXAudio2* live = PlayingEngineOtherThan(a_this))
					{
						LogForwarded(true, a_this, live);
						return g_origStartEngine(live);
					}
					return S_OK;
				}
			}
			return g_origStartEngine(a_this);
		}

		void STDMETHODCALLTYPE HookStopEngine(IXAudio2* a_this)
		{
			if (FromOtherThread())
			{
				g_otherModEngineState = 0;
				if (IsPinnedEngine(a_this))
				{
					std::scoped_lock l(g_pinLock);
					if (IXAudio2* live = PlayingEngineOtherThan(a_this))
					{
						LogForwarded(false, a_this, live);
						g_origStopEngine(live);
					}
					return;
				}
			}
			g_origStopEngine(a_this);
		}

		const char* OtherModEngineText()
		{
			switch (g_otherModEngineState.load())
			{
			case 0: return "stopped";
			case 1: return "started";
			default: return "none";
			}
		}

		HRESULT STDMETHODCALLTYPE HookGetDeviceCount(IXAudio2* a_this, UINT32* a_count)
		{
			const HRESULT hr = g_origDeviceCount(a_this, a_count);
			if (t_inside == 0 && g_deviceCountCalls.fetch_add(1) < 8)
			{
				logger::info("engine {} asked for its device count: {} device(s) ({})", Ptr(a_this), a_count ? *a_count : 0u, Hr(hr));
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE HookCreateMastering(IXAudio2* a_this, IXAudio2Voice** a_out, UINT32 a_channels, UINT32 a_rate, UINT32 a_flags, UINT32 a_index, void* a_chain)
		{
			// Only the game's own call is managed: it passes &BSXAudio2Audio::masteringVoice (+0x58) as the out pointer.
			void* object = a_out ? static_cast<void*>(reinterpret_cast<std::uint8_t*>(a_out) - kAudioMasterOff) : nullptr;
			if (t_inside > 0 || !IsGameAudioObject(object))
			{
				return g_origMaster(a_this, a_out, a_channels, a_rate, a_flags, a_index, a_chain);
			}

			const auto devices = ListXaDevices(a_this);
			logger::debug("the game creates its mastering voice: channels={} rate={} device index={}; XAudio2 lists {} device(s)", a_channels, a_rate, a_index, devices.size());
			for (const auto& d : devices) { logger::debug("  [{}] \"{}\" role=0x{:X} id={}", d.index, d.name, d.role, d.id); }

			std::string target;
			{
				std::scoped_lock l(g_swapLock);
				target = g_swapTargetId;
			}
			UINT32 pick = a_index;
			std::string why = "the game's choice (the Windows default device)";
			if (settings::general::enabled)
			{
				const Device* chosen = nullptr;
				if (!target.empty())
				{
					for (const auto& d : devices) { if (d.id == target) { chosen = &d; } }
					if (chosen) { why = "the switch target"; }
				}
				if (!chosen)
				{
					const std::string pref = Lower(Trim(settings::GetPreferredDevice()));
					if (!pref.empty())
					{
						for (const auto& d : devices) { if (!chosen && (d.id == pref || Lower(d.name).find(pref) != std::string::npos)) { chosen = &d; } }
						if (chosen) { why = "the preferred device"; }
					}
				}
				if (chosen) { pick = chosen->index; }
			}

			HRESULT hr = g_origMaster(a_this, a_out, a_channels, a_rate, a_flags, pick, a_chain);
			if (FAILED(hr) && pick != a_index)
			{
				logger::warn("device index {} refused a mastering voice ({}); using the game's choice", pick, Hr(hr));
				pick = a_index;
				why = "the game's choice (the chosen device refused)";
				hr = g_origMaster(a_this, a_out, a_channels, a_rate, a_flags, pick, a_chain);
			}
			if (FAILED(hr) || !*a_out)
			{
				logger::error("the game's mastering voice could not be created ({}): no usable output device", Hr(hr));
				std::scoped_lock l(g_stateLock);
				g_state.audio = object;
				g_state.engine = a_this;
				g_state.master = nullptr;
				g_state.deviceId.clear();
				g_state.deviceName.clear();
				return hr;
			}

			const HRESULT cb = a_this->RegisterForCallbacks(&g_callback);
			g_critical = false;
			std::string name, id;
			for (const auto& d : devices)
			{
				if (d.index == pick)
				{
					name = d.name;
					id = d.id;
				}
			}
			{
				std::scoped_lock l(g_stateLock);
				g_state.audio = object;
				g_state.engine = a_this;
				g_state.master = *a_out;
				g_state.deviceId = id;
				g_state.deviceName = name;
				g_state.channels = a_channels;
				g_state.rate = a_rate;
			}
			logger::info("the game's output is on [{}] \"{}\" ({}); engine {}, mastering voice {}, device-loss callback {}", pick, name, why, Ptr(a_this), Ptr(*a_out), Hr(cb));
			return hr;
		}

		// ---- the switch, on the game's audio thread ----
		void FinishSwap()
		{
			{
				std::scoped_lock l(g_swapLock);
				g_swapDone = true;
				g_swapTargetId.clear();
			}
			g_swapCv.notify_all();
		}

		void FailSwap(std::string a_text)
		{
			logger::error("{}", a_text);
			SetResult(std::move(a_text), true);
			FinishSwap();
		}

		void PerformSwap()
		{
			const auto start = std::chrono::steady_clock::now();
			g_swapStep = 1;
			void* audio = GameAudioObject();
			auto* mgr = RE::BSAudioManager::GetSingleton();
			if (!audio || !mgr)
			{
				FailSwap(std::format("switch skipped: the game's audio object {} or manager {} is not available", Ptr(audio), Ptr(mgr)));
				return;
			}
			std::string from;
			{
				std::scoped_lock l(g_stateLock);
				from = g_state.deviceName.empty() ? std::string("(no device)") : g_state.deviceName;
			}

			// 1. every live sound drops its source voice (destroyed while the engine still exists); the sound stays
			static void* sounds[2048];
			const int soundCount = SnapshotSounds(mgr, sounds, 2048);
			int detached = 0;
			for (int i = 0; i < soundCount; ++i)
			{
				if (void* voice = ReadPtr(sounds[i], kSoundVoiceOff))
				{
					DestroyVoiceSafe(voice);
					WritePtr(sounds[i], kSoundVoiceOff, nullptr);
					++detached;
				}
			}

			// 2. already-freed voices left in the game's own voice lists would crash its shutdown
			g_swapStep = 2;
			const int scrubbed = ScrubVoiceList(g_addr.voiceListA) + ScrubVoiceList(g_addr.voiceListB);

			// 3. the game's own shutdown and init; init creates the mastering voice through our hook on the target
			SehInfo seh{};
			g_swapStep = 3;
			// The game's shutdown calls into its engine without a null check; after a rebuild that could not create one
			// there is nothing to shut down.
			std::unique_lock pinGuard(g_pinLock);
			const bool repaired = CheckEngineCreateCall();
			g_inRebuild = true;
			if (void* oldEngine = ReadPtr(audio, kAudioEngineOff))
			{
				HoldEngine(static_cast<IUnknown*>(oldEngine), "the game's shutdown releases it");
				if (repaired) { PinEngine(static_cast<IUnknown*>(oldEngine)); }
				CallGameSlot(audio, kSlotShutdown, &seh);
			}
			else if (repaired)
			{
				for (IUnknown* e : std::vector<IUnknown*>(g_heldEngines)) { PinEngine(e); }
			}
			else { logger::info("the game has no audio engine (an earlier rebuild could not create one); its shutdown is skipped"); }
			{
				std::scoped_lock l(g_stateLock);
				g_state.engine = nullptr;
				g_state.master = nullptr;
			}
			if (seh.code)
			{
				RemoveSounds(mgr, false);
				g_inRebuild = false;
				LogFault("shutdown", seh);
				FailSwap(std::format("switch failed: the game's audio shutdown faulted (0x{:08X} at {}); sounds were dropped", seh.code, FrameText(reinterpret_cast<DWORD64>(seh.at))));
				return;
			}
			std::uint32_t poolWas = 0;
			const int mixersCleared = ClearOutputMixers(g_addr.voiceListA, g_addr.voiceListB, &poolWas);
			g_dropLogged = false;
			g_swapStep = 4;
			const int initResult = CallGameSlot(audio, kSlotInit, &seh);
			g_inRebuild = false;
			pinGuard.unlock();
			if (seh.code)
			{
				RemoveSounds(mgr, false);
				LogFault("init", seh);
				FailSwap(std::format("switch failed: the game's audio init faulted (0x{:08X} at {}); sounds were dropped", seh.code, FrameText(reinterpret_cast<DWORD64>(seh.at))));
				return;
			}
			if (!ReadPtr(audio, kAudioEngineOff))
			{
				const int dropped = RemoveSounds(mgr, false);
				FailSwap(std::format("the game's audio init could not create an audio engine (was \"{}\"); {} sound(s) dropped; new sounds are skipped until a switch succeeds", from, dropped));
				return;
			}
			if (!ReadPtr(audio, kAudioMasterOff))
			{
				const int dropped = RemoveSounds(mgr, false);
				FailSwap(std::format("no device accepted the rebuilt audio (was \"{}\"); {} sound(s) dropped; waiting for a device", from, dropped));
				return;
			}

			ReleaseHeldEngines(static_cast<IUnknown*>(ReadPtr(audio, kAudioEngineOff)));
			if (g_otherModEngineState.load() == 0 && g_origStopEngine)
			{
				if (auto* rebuilt = static_cast<IXAudio2*>(ReadPtr(audio, kAudioEngineOff)))
				{
					g_origStopEngine(rebuilt);
					logger::info("the rebuilt engine is stopped as well: another mod had stopped the game's sound (the game is in the background)");
				}
			}
			g_swapStep = 5;
			// 4. the game's own per-sound voice setup rebuilds every surviving sound on the new engine
			int revived = 0;
			int failed = 0;
			for (int i = 0; i < soundCount; ++i)
			{
				if (ReadPtr(sounds[i], kSoundVoiceOff)) { ++revived; continue; }
				if (CallSetupSound(g_addr.setupSound, sounds[i]) && ReadPtr(sounds[i], kSoundVoiceOff)) { ++revived; }
				else { ++failed; }
			}
			const int removed = RemoveSounds(mgr, true);
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			{
				std::scoped_lock l(g_stateLock);
				++g_state.switches;
				g_state.lastResult = std::format("switched \"{}\" -> \"{}\" in {} ms: {} sound(s) revived, {} failed, {} removed; {} voice(s) detached, {} stale list entr{} cleared, {} output mixer slot(s) and {} spare(s) emptied, init {}",
					from, g_state.deviceName, ms, revived, failed, removed, detached, scrubbed, scrubbed == 1 ? "y" : "ies", mixersCleared, poolWas, initResult);
				logger::info("{}", g_state.lastResult);
			}
			FinishSwap();
		}

		void HookProcessSounds(void* a_manager)
		{
			g_audioTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
			g_lastEnterMs.store(NowMs(), std::memory_order_relaxed);
			g_iterations.fetch_add(1, std::memory_order_relaxed);
			if (g_swapPending.exchange(false))
			{
				PerformSwap();
				g_swapStep = 0;
			}
			g_origProcessSounds(a_manager);
			g_lastExitMs.store(NowMs(), std::memory_order_relaxed);
		}

		bool PatchSlot(void** a_vtable, std::size_t a_slot, void* a_hook, void** a_original, const char* a_name)
		{
			void** entry = a_vtable + a_slot;
			if (*entry == a_hook) { return true; }
			DWORD old = 0;
			if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old))
			{
				logger::error("could not unprotect the vtable slot for {} (error {})", a_name, GetLastError());
				return false;
			}
			*a_original = *entry;
			*entry = a_hook;
			VirtualProtect(entry, sizeof(void*), old, &old);
			logger::info("hooked {} (vtable slot {}, original {})", a_name, a_slot, Ptr(*a_original));
			return true;
		}

		// ---- the game never builds a sound's voice on a missing engine ----
		// The game builds each new sound's source voice (SE 66708 / AE 67955) through BSXAudio2Audio+0x50 with no null check,
		// and its shutdown nulls that field while sound stays switched on (+0x40). When a rebuild's init cannot create an
		// engine (IXAudio2::Initialize failing while a device changes), the next sound crashed there - a Nexus report on AE
		// 1.6.1170, 67955+0x231 (falsification episode 47). Its one call, in the per-sound setup, gets no voice instead, which
		// the game already handles by dropping that sound, and a switch is requested at most every 5 s so audio comes back
		// once a device takes it.
		using BuildVoiceFn = void* (*)(void*, void*, void*, void*, std::uint8_t);
		BuildVoiceFn g_origBuildVoice = nullptr;

		void* HookBuildVoice(void* a_audio, void* a_desc, void* a_owner, void* a_extra, std::uint8_t a_flag)
		{
			if (a_audio && !ReadPtr(a_audio, kAudioEngineOff))
			{
				const auto n = g_nullEngineSkips.fetch_add(1) + 1;
				if (n == 1 || n % 500 == 0)
				{
					logger::warn("the game started a sound while it has no audio engine; the sound was skipped instead of crashing ({} so far)", n);
				}
				const std::int64_t now = NowMs();
				std::int64_t last = g_lastNullEngineResetMs.load();
				if (now - last >= 5000 && g_lastNullEngineResetMs.compare_exchange_strong(last, now))
				{
					RequestReset("the game has no audio engine", true, true);
				}
				return nullptr;
			}
			return g_origBuildVoice(a_audio, a_desc, a_owner, a_extra, a_flag);
		}

		void InstallVoiceGuard()
		{
			const auto* setup = reinterpret_cast<const std::uint8_t*>(g_addr.setupSound);
			std::uintptr_t site = 0;
			for (std::uintptr_t off = 0; off < 0x1A0 && !site; ++off)
			{
				if (setup[off] != 0xE8) { continue; }
				std::int32_t rel = 0;
				std::memcpy(&rel, setup + off + 1, sizeof(rel));
				if (g_addr.setupSound + off + 5 + static_cast<std::intptr_t>(rel) == g_addr.buildVoice) { site = g_addr.setupSound + off; }
			}
			if (!site)
			{
				g_voiceGuardResult = "the call that builds a sound's voice was not found in the per-sound setup; sounds are not guarded against a missing engine";
				logger::warn("{}", g_voiceGuardResult);
				return;
			}
			g_origBuildVoice = reinterpret_cast<BuildVoiceFn>(SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&HookBuildVoice)));
			g_voiceGuardResult = std::format("guarded the per-sound setup's voice call at +0x{:X}", site - g_addr.setupSound);
			logger::info("{}", g_voiceGuardResult);
		}

		bool InstallThreadHook()
		{
			// Find the call to the per-pass sound processing inside the audio thread's run loop by its target, so the
			// instruction offset may differ between runtimes (+0x57 on 1.5.97, +0x58 on 1.7.104).
			const auto* loop = reinterpret_cast<const std::uint8_t*>(g_addr.threadLoop);
			std::uintptr_t site = 0;
			for (std::uintptr_t off = 0; off < 0x100 && !site; ++off)
			{
				if (loop[off] != 0xE8) { continue; }
				std::int32_t rel = 0;
				std::memcpy(&rel, loop + off + 1, sizeof(rel));
				if (g_addr.threadLoop + off + 5 + static_cast<std::intptr_t>(rel) == g_addr.processSounds) { site = g_addr.threadLoop + off; }
			}
			if (!site)
			{
				g_threadHookResult = "the call to the sound processing was not found in the audio thread loop; switching disabled";
				logger::error("{}", g_threadHookResult);
				return false;
			}
			// the trampoline is allocated once in SKSEPluginLoad (main.cpp), shared with the media keys hook
			auto& trampoline = SKSE::GetTrampoline();
			g_origProcessSounds = reinterpret_cast<ProcessSoundsFn>(trampoline.write_call<5>(site, reinterpret_cast<std::uintptr_t>(&HookProcessSounds)));
			g_threadHookResult = std::format("hooked the audio thread's sound-processing call at +0x{:X}", site - g_addr.threadLoop);
			logger::info("{}", g_threadHookResult);
			return g_origProcessSounds != nullptr;
		}

		// ---- diagnostics: stacks of the audio thread and of every thread inside the audio engine ----
		std::uint64_t ReadValueSafe(void* a_base, std::uintptr_t a_off, int a_size)
		{
			if (!a_base) { return ~0ull; }
			__try
			{
				const auto* p = static_cast<std::uint8_t*>(a_base) + a_off;
				if (a_size == 1) { return *p; }
				if (a_size == 4) { std::uint32_t v = 0; std::memcpy(&v, p, 4); return v; }
				std::uint64_t v = 0;
				std::memcpy(&v, p, 8);
				return v;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return ~0ull; }
		}

		struct StackFrames
		{
			DWORD64 pc[24];
			int count;
		};

		// The thread is suspended by the caller. No allocation and no logging here: the suspended thread may hold the
		// heap or the logger's lock.
		int WalkSuspended(HANDLE a_thread, StackFrames* a_out)
		{
			a_out->count = 0;
			CONTEXT ctx;
			std::memset(&ctx, 0, sizeof(ctx));
			ctx.ContextFlags = CONTEXT_FULL;
			if (!GetThreadContext(a_thread, &ctx)) { return -1; }
			__try
			{
				for (int i = 0; i < 24 && ctx.Rip; ++i)
				{
					a_out->pc[a_out->count++] = ctx.Rip;
					DWORD64 imageBase = 0;
					PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
					if (!fn)
					{
						ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
						ctx.Rsp += 8;
						continue;
					}
					PVOID handlerData = nullptr;
					DWORD64 establisher = 0;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return a_out->count;
		}

		bool CaptureStack(DWORD a_tid, StackFrames& a_out)
		{
			a_out.count = 0;
			if (a_tid == 0 || a_tid == GetCurrentThreadId()) { return false; }
			HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_tid);
			if (!h) { return false; }
			bool ok = false;
			if (SuspendThread(h) != static_cast<DWORD>(-1))
			{
				ok = WalkSuspended(h, &a_out) > 0;
				ResumeThread(h);
			}
			CloseHandle(h);
			return ok;
		}

		std::string FrameText(DWORD64 a_pc)
		{
			HMODULE mod = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_pc), &mod) && mod)
			{
				wchar_t path[MAX_PATH]{};
				GetModuleFileNameW(mod, path, MAX_PATH);
				const wchar_t* base = path;
				for (const wchar_t* c = path; *c; ++c) { if (*c == L'\\' || *c == L'/') { base = c + 1; } }
				return std::format("{}+{:X}", Narrow(base), a_pc - reinterpret_cast<DWORD64>(mod));
			}
			return std::format("{:X}", a_pc);
		}

		std::string StackText(const StackFrames& a_frames, bool* a_inAudio = nullptr)
		{
			std::string out;
			bool audio = false;
			for (int i = 0; i < a_frames.count; ++i)
			{
				const std::string frame = FrameText(a_frames.pc[i]);
				const std::string low = Lower(frame);
				audio = audio || low.find("xaudio2") != std::string::npos || low.find("audioses") != std::string::npos || low.find("xapofx") != std::string::npos;
				if (!out.empty()) { out += " <- "; }
				out += frame;
			}
			if (a_inAudio) { *a_inAudio = audio; }
			return out;
		}

		void LogFault(const char* a_where, const SehInfo& a_seh)
		{
			StackFrames frames{};
			frames.count = a_seh.frameCount;
			for (int i = 0; i < a_seh.frameCount && i < 24; ++i) { frames.pc[i] = a_seh.frames[i]; }
			const char* kind = a_seh.accessKind == 1 ? "writing" : (a_seh.accessKind == 8 ? "executing" : "reading");
			logger::error("the game's audio {} faulted: 0x{:08X} at {} {} 0x{:X}; stack: {}", a_where, a_seh.code, FrameText(reinterpret_cast<DWORD64>(a_seh.at)), kind,
						  a_seh.accessTarget, StackText(frames));
		}

		void ReportAudioThread(std::string_view a_why)
		{
			const auto now = NowMs();
			auto age = [now](std::int64_t a_t) { return a_t ? now - a_t : -1; };
			void* mgr = RE::BSAudioManager::GetSingleton();
			void* thread = reinterpret_cast<void*>(ReadValueSafe(mgr, 0xF8, 8));
			const DWORD tid = g_audioTid.load();
			const auto enter = g_lastEnterMs.load();
			const auto exit = g_lastExitMs.load();
			logger::warn("audio thread watch ({}): thread {} (manager says {}), {} pass(es); entered the sound processing {} ms ago, left it {} ms ago - {}; switch step {}; engine passes {} (last {} ms ago); critical errors {}; thread object {} busy@48={} sync@60={} stop@61={}",
				a_why, tid, ReadValueSafe(mgr, 0xF4, 4), g_iterations.load(), age(enter), age(exit), enter > exit ? "INSIDE the sound processing" : "between passes",
				g_swapStep.load(), g_passes.load(), age(g_lastPassMs.load()), g_criticalCount.load(), Ptr(thread),
				ReadValueSafe(thread, 0x48, 1), ReadValueSafe(thread, 0x60, 1), ReadValueSafe(thread, 0x61, 1));

			StackFrames frames{};
			if (CaptureStack(tid, frames)) { logger::warn("audio thread {} stack: {}", tid, StackText(frames)); }
			else { logger::warn("audio thread {} stack: could not be captured", tid); }

			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) { return; }
			THREADENTRY32 te{};
			te.dwSize = sizeof(te);
			const DWORD pid = GetCurrentProcessId();
			int scanned = 0;
			int inAudio = 0;
			for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == tid || te.th32ThreadID == GetCurrentThreadId()) { continue; }
				++scanned;
				StackFrames f{};
				if (!CaptureStack(te.th32ThreadID, f)) { continue; }
				bool audio = false;
				const std::string text = StackText(f, &audio);
				if (audio)
				{
					++inAudio;
					logger::warn("thread {} (in the audio engine or audio session) stack: {}", te.th32ThreadID, text);
				}
			}
			CloseHandle(snap);
			logger::warn("audio thread watch: {} other thread(s) scanned, {} inside the audio engine or audio session", scanned, inAudio);
		}

		// ---- the decision, on the worker thread ----
		std::mutex g_wakeLock;  // guards the fields below
		std::condition_variable g_wakeCv;
		bool g_pending{ false };
		bool g_force{ false };
		bool g_urgent{ false };
		bool g_busy{ false };
		std::string g_avoidId;
		std::string g_pendingReason;
		std::string g_lastReason{ "none" };
		std::string g_watcherResult{ "not started" };
		std::chrono::steady_clock::time_point g_lastEvent{};
		std::uint32_t g_requests{ 0 };
		std::uint32_t g_runs{ 0 };

		// bSwitchToNewDevice (the owner, 2026-09-14: "allow users to connect their headset after the game started"): endpoints
		// that became active while the game runs, newest last. Render and capture ids alike - Evaluate keeps only the ones in
		// its active render list. Cleared once a check has acted on one (or the preferred device outranked it).
		std::mutex g_arrivalLock;
		std::vector<std::string> g_arrivals;

		void NoteArrival(const std::string& a_id)
		{
			std::scoped_lock l(g_arrivalLock);
			std::erase(g_arrivals, a_id);
			g_arrivals.push_back(a_id);
			if (g_arrivals.size() > 8) { g_arrivals.erase(g_arrivals.begin()); }
		}

		void Evaluate(const std::string& a_reason, bool a_force, const std::string& a_avoidId)
		{
			State snap;
			{
				std::scoped_lock l(g_stateLock);
				snap = g_state;
			}
			if (!g_threadHooked)
			{
				SetResult("switching is disabled: the audio thread could not be hooked", true);
				return;
			}
			if (!GameAudioObject())
			{
				logger::debug("switch check ({}) before the game created its audio", a_reason);
				return;
			}

			auto active = ListEndpoints(true);
			if (!a_avoidId.empty())
			{
				const std::string avoid = Lower(a_avoidId);
				std::erase_if(active, [&](const Endpoint& e) { return e.id == avoid; });
			}
			const Endpoint* preferred = PreferredEndpoint(active);
			const Endpoint* def = nullptr;
			for (const auto& e : active) { if (e.isDefault) { def = &e; } }
			// A device connected while playing: the newest arrival that is an active output now.
			const Endpoint* arrived = nullptr;
			if (settings::general::switchToNewDevice)
			{
				std::scoped_lock l(g_arrivalLock);
				for (auto it = g_arrivals.rbegin(); it != g_arrivals.rend() && !arrived; ++it)
				{
					for (const auto& e : active)
					{
						if (e.id == *it)
						{
							arrived = &e;
							break;
						}
					}
				}
			}
			const Endpoint* target = preferred ? preferred : (arrived ? arrived : (def ? def : (active.empty() ? nullptr : &active.front())));
			const bool currentActive = std::any_of(active.begin(), active.end(), [&](const Endpoint& e) { return e.id == snap.deviceId; });
			const bool critical = g_critical.load();

			bool swap = false;
			std::string decision;
			if (!target) { decision = "no output device is connected; waiting for one"; }
			else if (a_force) { swap = true; decision = "forced"; }
			else if (critical) { swap = true; decision = "the engine lost its device"; }
			else if (!snap.master) { swap = true; decision = "the game has no output yet"; }
			else if (!currentActive) { swap = true; decision = "the current device is gone"; }
			else if (preferred) { swap = snap.deviceId != preferred->id; decision = swap ? "the preferred device is available" : "already on the preferred device"; }
			else if (arrived) { swap = snap.deviceId != arrived->id; decision = swap ? "an output device was connected while playing" : "already on the device connected while playing"; }
			else if (settings::general::switchOnDefaultChange) { swap = snap.deviceId != target->id; decision = swap ? "the Windows default device changed" : "already on the Windows default device"; }
			else { decision = "the current device is still connected and following the Windows default is off"; }

			logger::info("switch check ({}): on \"{}\", target \"{}\", critical={}: {}", a_reason, snap.deviceName.empty() ? "(none)" : snap.deviceName,
						 target ? target->name : "(none)", critical, decision);
			if (arrived || preferred)
			{
				std::scoped_lock l(g_arrivalLock);
				g_arrivals.clear();   // acted on; a later Windows default change still counts
			}
			if (!swap)
			{
				SetResult(decision);
				return;
			}

			// The device the engine plays on went away and XAudio2 has not reported it yet: its own device-removal handling is
			// still running, and DestroyVoice on that engine meanwhile deadlocks the game's audio thread against XAudio2's
			// threads for good (falsification episode 46, three real wired-headset unplugs on SE 1.5.97: the one whose critical
			// error arrived before the switch took 40 ms; the two whose switch started first hung in DestroyVoice with no
			// critical error ever arriving). So the switch waits for the engine's report first. The extra 100 ms after it is a
			// margin, not a measured need.
			if (!currentActive && snap.master && !g_critical.load())
			{
				const auto waitStart = std::chrono::steady_clock::now();
				while (!g_critical.load() && std::chrono::steady_clock::now() - waitStart < std::chrono::milliseconds(3000))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
				if (g_critical.load())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					logger::info("waited {} ms for the audio engine to finish handling the removed device, then 100 ms more", waited);
				}
				else
				{
					logger::warn("the audio engine did not report the removed device within {} ms; switching anyway", waited);
				}
			}

			bool ready = false;
			for (int attempt = 0; attempt < 10 && !ready; ++attempt)
			{
				ready = EndpointReady(target->id);
				if (!ready)
				{
					logger::info("\"{}\" is not taking audio yet (attempt {}); waiting", target->name, attempt + 1);
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			}
			if (!ready)
			{
				SetResult(std::format("\"{}\" never started taking audio; no switch", target->name), true);
				return;
			}

			bool timedOut = false;
			{
				std::unique_lock l(g_swapLock);
				g_swapTargetId = target->id;
				g_swapDone = false;
				g_swapPending = true;
				bool reported = false;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (!g_swapDone)
				{
					const auto step = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(2));
					if (g_swapCv.wait_until(l, step, [] { return g_swapDone; })) { break; }
					if (std::chrono::steady_clock::now() >= deadline)
					{
						g_swapPending = false;
						g_swapTargetId.clear();
						timedOut = true;
						break;
					}
					if (!reported)
					{
						reported = true;
						l.unlock();
						ReportAudioThread("a switch has waited 2 s for the audio thread");
						l.lock();
					}
				}
			}
			if (timedOut)
			{
				ReportAudioThread("the switch timed out after 10 s");
				SetResult("the game's audio thread did not run the switch within 10 s", true);
				logger::warn("the game's audio thread did not run the switch within 10 s");
			}
		}

		class NotificationClient final : public IMMNotificationClient
		{
		public:
			ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
			ULONG STDMETHODCALLTYPE Release() override { return 1; }

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_iid, void** a_out) override
			{
				if (!a_out) { return E_POINTER; }
				if (a_iid == IID_IUnknown || a_iid == __uuidof(IMMNotificationClient))
				{
					*a_out = static_cast<IMMNotificationClient*>(this);
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}

			HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR a_id, DWORD a_state) override
			{
				const std::string id = Lower(Narrow(a_id));
				bool leaving = false;
				{
					std::scoped_lock l(g_stateLock);
					leaving = a_state != DEVICE_STATE_ACTIVE && !g_state.deviceId.empty() && g_state.deviceId == id;
				}
				if (a_state == DEVICE_STATE_ACTIVE) { NoteArrival(id); }
				RequestReset(std::format("endpoint {} is now {}", id, EndpointStateName(a_state)), false, leaving, leaving ? id : std::string());
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR a_id) override
			{
				NoteArrival(Lower(Narrow(a_id)));
				RequestReset(std::format("endpoint {} added", Lower(Narrow(a_id))), false);
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR a_id) override
			{
				const std::string id = Lower(Narrow(a_id));
				bool leaving = false;
				{
					std::scoped_lock l(g_stateLock);
					leaving = !g_state.deviceId.empty() && g_state.deviceId == id;
				}
				RequestReset(std::format("endpoint {} removed", id), false, leaving, leaving ? id : std::string());
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow a_flow, ERole a_role, LPCWSTR a_id) override
			{
				if (a_flow == eRender && a_role == eConsole)
				{
					RequestReset(std::format("the Windows default output changed to {}", a_id ? Lower(Narrow(a_id)) : std::string("(none)")), false);
				}
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
		};

		NotificationClient g_client;

		void WorkerMain()
		{
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			IMMDeviceEnumerator* enumerator = nullptr;
			HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
			if (SUCCEEDED(hr) && enumerator) { hr = enumerator->RegisterEndpointNotificationCallback(&g_client); }
			std::string watcher = SUCCEEDED(hr) ? std::string("watching Windows audio endpoints") :
			                                      std::format("endpoint notifications unavailable ({}); only device loss and the tool trigger switches", Hr(hr));
			{
				std::scoped_lock l(g_wakeLock);
				g_watcherResult = watcher;
			}
			logger::info("worker started (COM {}): {}", Hr(co), watcher);

			for (;;)
			{
				std::string reason;
				std::string avoid;
				bool force = false;
				{
					std::unique_lock l(g_wakeLock);
					g_wakeCv.wait(l, [] { return g_pending; });
					while (!g_urgent)
					{
						const auto due = g_lastEvent + std::chrono::milliseconds(settings::general::resetDelayMs.load());
						if (std::chrono::steady_clock::now() >= due) { break; }
						g_wakeCv.wait_until(l, due);
					}
					reason = std::move(g_pendingReason);
					avoid = std::move(g_avoidId);
					force = g_force;
					g_pendingReason.clear();
					g_avoidId.clear();
					g_pending = false;
					g_force = false;
					g_urgent = false;
					g_busy = true;
					g_lastReason = reason;
					++g_runs;
				}
				if (settings::general::enabled) { Evaluate(reason, force, avoid); }
				else { logger::debug("switch check ({}) skipped: bEnabled is off", reason); }
				{
					std::scoped_lock l(g_wakeLock);
					g_busy = false;
				}
				g_wakeCv.notify_all();
			}
		}
	}

	void Install()
	{
		g_addr.threadLoop = REL::RelocationID(66482, 67746).address();
		g_addr.processSounds = REL::RelocationID(66461, 67725).address();
		g_addr.setupSound = REL::RelocationID(66762, 68003).address();
		g_addr.voiceListA = REL::RelocationID(511864, 388390).address();
		g_addr.voiceListB = REL::RelocationID(511867, 388393).address();
		g_addr.audioObject = REL::RelocationID(523613, 410149).address();
		g_addr.audioVtable = REL::RelocationID(285056, 236527).address();
		g_addr.buildVoice = REL::RelocationID(66708, 67955).address();
		g_addr.createCall = REL::RelocationID(66746, 67952).address() + REL::Relocate(0x44, 0xB7);
		logger::info("game addresses: thread loop {:X}, sound processing {:X}, sound setup {:X}, voice lists {:X}/{:X}, audio object {:X}, audio vtable {:X}",
					 g_addr.threadLoop, g_addr.processSounds, g_addr.setupSound, g_addr.voiceListA, g_addr.voiceListB, g_addr.audioObject, g_addr.audioVtable);
		InstallVoiceGuard();
		CheckEngineCreateCall();
		logger::info("the game's engine-creation call at load: {}", g_createCallState);

		HMODULE module = LoadLibraryW(L"XAudio2_7.dll");
		if (!module)
		{
			g_installResult = std::format("XAudio2_7.dll could not be loaded (error {}); nothing hooked", GetLastError());
			logger::error("{}", g_installResult);
			return;
		}
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"XAudio2_7.dll", &pinned);  // the game frees it on every rebuild
		InstallLeapGuard(module);

		void** vtable = nullptr;
		HRESULT created = E_FAIL;
		std::thread([&]() {
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			IXAudio2* probe = nullptr;
			created = CoCreateInstance(CLSID_XAudio2, nullptr, CLSCTX_INPROC_SERVER, IID_IXAudio2, reinterpret_cast<void**>(&probe));
			if (SUCCEEDED(created) && probe)
			{
				vtable = *reinterpret_cast<void***>(probe);
				probe->Release();
			}
			if (SUCCEEDED(co)) { CoUninitialize(); }
		}).join();
		if (FAILED(created) || !vtable)
		{
			g_installResult = std::format("the XAudio2 2.7 engine could not be created ({}); nothing hooked", Hr(created));
			logger::error("{}", g_installResult);
			return;
		}
		MEMORY_BASIC_INFORMATION info{};
		if (!VirtualQuery(vtable, &info, sizeof(info)) || info.AllocationBase != static_cast<void*>(module))
		{
			g_installResult = std::format("the IXAudio2 vtable {} is not inside XAudio2_7.dll; nothing hooked", Ptr(vtable));
			logger::error("{}", g_installResult);
			return;
		}

		bool ok = true;
		ok &= PatchSlot(vtable, slot::kCreateMasteringVoice, reinterpret_cast<void*>(&HookCreateMastering), reinterpret_cast<void**>(&g_origMaster), "IXAudio2::CreateMasteringVoice");
		ok &= PatchSlot(vtable, slot::kInitialize, reinterpret_cast<void*>(&HookInitialize), reinterpret_cast<void**>(&g_origInitialize), "IXAudio2::Initialize");
		ok &= PatchSlot(vtable, slot::kGetDeviceCount, reinterpret_cast<void*>(&HookGetDeviceCount), reinterpret_cast<void**>(&g_origDeviceCount), "IXAudio2::GetDeviceCount");
		ok &= PatchSlot(vtable, slot::kCreateSourceVoice, reinterpret_cast<void*>(&HookCreateSource), reinterpret_cast<void**>(&g_origCreateSource), "IXAudio2::CreateSourceVoice");
		ok &= PatchSlot(vtable, slot::kCreateSubmixVoice, reinterpret_cast<void*>(&HookCreateSubmix), reinterpret_cast<void**>(&g_origCreateSubmix), "IXAudio2::CreateSubmixVoice");
		if (!PatchSlot(vtable, slot::kStartEngine, reinterpret_cast<void*>(&HookStartEngine), reinterpret_cast<void**>(&g_origStartEngine), "IXAudio2::StartEngine") ||
			!PatchSlot(vtable, slot::kStopEngine, reinterpret_cast<void*>(&HookStopEngine), reinterpret_cast<void**>(&g_origStopEngine), "IXAudio2::StopEngine"))
		{
			logger::warn("StartEngine/StopEngine could not be hooked; another mod's engine calls are not forwarded after a switch");
		}
		g_hooked = ok && g_origMaster && g_origInitialize && g_origDeviceCount && g_origCreateSource && g_origCreateSubmix;
		g_threadHooked = g_hooked && InstallThreadHook();
		g_installResult = g_hooked ? std::format("hooked (XAudio2_7.dll at {}, vtable {})", Ptr(module), Ptr(vtable)) : std::string("one or more vtable slots could not be hooked");
		logger::info("install: {}; {}", g_installResult, g_threadHookResult);
	}

	void StartWatcher()
	{
		if (!g_hooked)
		{
			logger::warn("not starting the worker: the XAudio2 hooks are not installed");
			return;
		}
		std::thread(WorkerMain).detach();
	}

	void RequestReset(std::string_view a_reason, bool a_force, bool a_urgent, std::string_view a_avoidId)
	{
		{
			std::scoped_lock l(g_wakeLock);
			if (g_pendingReason.size() < 1024)
			{
				if (!g_pendingReason.empty()) { g_pendingReason += "; "; }
				g_pendingReason += a_reason;
			}
			g_pending = true;
			g_force = g_force || a_force;
			g_urgent = g_urgent || a_urgent;
			if (!a_avoidId.empty()) { g_avoidId = std::string(a_avoidId); }
			g_lastEvent = std::chrono::steady_clock::now();
			++g_requests;
		}
		g_wakeCv.notify_all();
	}

	bool WaitIdle(int a_timeoutMs)
	{
		std::unique_lock l(g_wakeLock);
		return g_wakeCv.wait_for(l, std::chrono::milliseconds(a_timeoutMs), [] { return !g_pending && !g_busy; });
	}

	Status GetStatus()
	{
		Status s;
		s.hooked = g_hooked && g_threadHooked;
		std::scoped_lock l(g_stateLock);
		s.managed = g_state.audio != nullptr;
		s.attached = g_state.master != nullptr;
		s.critical = g_critical.load();
		s.device = g_state.deviceName;
		s.resets = g_state.switches;
		s.lastResult = g_state.lastResult;
		return s;
	}

	std::string CurrentDeviceId()
	{
		std::scoped_lock l(g_stateLock);
		return g_state.master ? g_state.deviceId : std::string();
	}

	std::vector<std::string> DeviceNames()
	{
		static std::mutex cacheLock;
		static std::vector<std::string> cached;
		static std::chrono::steady_clock::time_point refreshed{};
		std::scoped_lock c(cacheLock);
		const auto now = std::chrono::steady_clock::now();
		if (now - refreshed >= std::chrono::seconds(2))
		{
			refreshed = now;
			cached.clear();
			for (const auto& e : ListEndpoints(true)) { cached.push_back(e.name); }
		}
		return cached;
	}

	void FailNextEngineInit()
	{
		g_failNextInit = true;
		logger::info("DevBench tool: the next engine initialisation will fail on purpose");
	}

	std::string StateJson()
	{
		State snap;
		{
			std::scoped_lock l(g_stateLock);
			snap = g_state;
		}
		std::scoped_lock l(g_wakeLock);
		return std::format(
			"\"hooks\":{{\"installed\":{},\"result\":\"{}\",\"audioThread\":{},\"audioThreadResult\":\"{}\",\"initializeCalls\":{},\"deviceCountCalls\":{},\"voiceGuard\":\"{}\",\"soundsSkippedNoEngine\":{},\"heldEngines\":{},\"pinnedEngines\":{},\"createCall\":\"{}\",\"otherModEngine\":\"{}\",\"xaudio2DeadlockGuard\":\"{}\",\"xaudio2DeadlocksAvoided\":{}}},"
			"\"worker\":{{\"result\":\"{}\",\"pending\":{},\"busy\":{},\"swapPending\":{},\"requests\":{},\"runs\":{},\"lastReason\":\"{}\"}},"
			"\"engines\":[{{\"audioObject\":\"{}\",\"engine\":\"{}\",\"master\":\"{}\",\"attached\":{},\"device\":\"{}\",\"deviceId\":\"{}\",\"channels\":{},\"rate\":{},"
			"\"passes\":{},\"critical\":{},\"criticalErrors\":{},\"resets\":{},\"failures\":{},\"lastResult\":\"{}\"}}]",
			g_hooked.load() ? "true" : "false", EscapeJson(g_installResult), g_threadHooked.load() ? "true" : "false", EscapeJson(g_threadHookResult),
			g_initializeCalls.load(), g_deviceCountCalls.load(), EscapeJson(g_voiceGuardResult), g_nullEngineSkips.load(), g_heldCount.load(), g_pinnedEngines.size(), EscapeJson(g_createCallState), OtherModEngineText(), EscapeJson(g_leapGuardResult), g_leapCollisions.load(), EscapeJson(g_watcherResult), g_pending ? "true" : "false", g_busy ? "true" : "false",
			g_swapPending.load() ? "true" : "false", g_requests, g_runs, EscapeJson(g_lastReason), Ptr(snap.audio), Ptr(snap.engine), Ptr(snap.master),
			snap.master ? "true" : "false", EscapeJson(snap.deviceName), EscapeJson(snap.deviceId), snap.channels, snap.rate, g_passes.load(),
			g_critical.load() ? "true" : "false", g_criticalCount.load(), snap.switches, snap.failures, EscapeJson(snap.lastResult));
	}

	std::string DevicesJson()
	{
		std::string endpoints;
		for (const auto& e : ListEndpoints(false))
		{
			if (!endpoints.empty()) { endpoints += ","; }
			endpoints += std::format("{{\"name\":\"{}\",\"id\":\"{}\",\"state\":\"{}\",\"default\":{}}}", EscapeJson(e.name), EscapeJson(e.id),
				EndpointStateName(e.state), e.isDefault ? "true" : "false");
		}
		return std::format("\"windowsEndpoints\":[{}]", endpoints);
	}

	void LogSummary(std::string_view a_when)
	{
		State snap;
		{
			std::scoped_lock l(g_stateLock);
			snap = g_state;
		}
		logger::info("{}: hooks {}, audio thread {}; game audio object {}; on \"{}\"; {} switch(es), last: {}", a_when, g_hooked ? "installed" : "NOT installed",
					 g_threadHooked ? "hooked" : "NOT hooked", Ptr(snap.audio), snap.deviceName, snap.switches, snap.lastResult);
		if (g_hooked && !snap.audio)
		{
			logger::warn("{}: the game has not created its audio output through the hooks - no output device was usable when it started", a_when);
		}
	}
}
