#pragma once

// Auto Audio Output Switch - own code, GPL-3.0-or-later (2026-09-13).
// The XAudio2 2.7 COM interfaces Skyrim uses on every runtime (XAudio2_7.dll), declared from their published ABI:
// method order, the pack(1) structures and the 2.7 CLSID/IID. The Windows SDK only ships XAudio2 2.8+, whose
// interfaces differ, and the June 2010 DirectX SDK is not a build dependency of this project.

#include <Windows.h>
#include <mmreg.h>

namespace xa27
{
	inline constexpr CLSID CLSID_XAudio2 = { 0x5a508685, 0xa254, 0x4fba, { 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf } };
	inline constexpr IID IID_IXAudio2 = { 0x8bcf1f58, 0x9fe7, 0x4583, { 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb } };

	inline constexpr UINT32 kDefaultGameDevice = 8;       // XAUDIO2_DEVICE_ROLE DefaultGameDevice
	inline constexpr UINT32 kAnyProcessor = 0xFFFFFFFF;   // XAUDIO2_ANY_PROCESSOR

	struct IXAudio2Voice;

#pragma pack(push, 1)
	struct DeviceDetails
	{
		WCHAR DeviceID[256];
		WCHAR DisplayName[256];
		UINT32 Role;
		WAVEFORMATEXTENSIBLE OutputFormat;
	};

	struct VoiceDetails
	{
		UINT32 CreationFlags;
		UINT32 InputChannels;
		UINT32 InputSampleRate;
	};

	struct SendDescriptor
	{
		UINT32 Flags;
		IXAudio2Voice* pOutputVoice;
	};

	struct VoiceSends
	{
		UINT32 SendCount;
		SendDescriptor* pSends;
	};
#pragma pack(pop)

	struct IXAudio2EngineCallback
	{
		virtual void STDMETHODCALLTYPE OnProcessingPassStart() = 0;
		virtual void STDMETHODCALLTYPE OnProcessingPassEnd() = 0;
		virtual void STDMETHODCALLTYPE OnCriticalError(HRESULT a_error) = 0;
	};

	// IXAudio2Voice has no IUnknown base. Mastering and submix voices add no methods in 2.7.
	struct IXAudio2Voice
	{
		virtual void STDMETHODCALLTYPE GetVoiceDetails(VoiceDetails* a_details) = 0;     // 0
		virtual HRESULT STDMETHODCALLTYPE SetOutputVoices(const VoiceSends* a_sends) = 0;  // 1
		virtual HRESULT STDMETHODCALLTYPE SetEffectChain(void*) = 0;                       // 2
		virtual HRESULT STDMETHODCALLTYPE EnableEffect(UINT32, UINT32) = 0;                // 3
		virtual HRESULT STDMETHODCALLTYPE DisableEffect(UINT32, UINT32) = 0;               // 4
		virtual void STDMETHODCALLTYPE GetEffectState(UINT32, BOOL*) = 0;                  // 5
		virtual HRESULT STDMETHODCALLTYPE SetEffectParameters(UINT32, const void*, UINT32, UINT32) = 0;  // 6
		virtual HRESULT STDMETHODCALLTYPE GetEffectParameters(UINT32, void*, UINT32) = 0;  // 7
		virtual HRESULT STDMETHODCALLTYPE SetFilterParameters(const void*, UINT32) = 0;    // 8
		virtual void STDMETHODCALLTYPE GetFilterParameters(void*) = 0;                     // 9
		virtual HRESULT STDMETHODCALLTYPE SetOutputFilterParameters(IXAudio2Voice*, const void*, UINT32) = 0;  // 10
		virtual void STDMETHODCALLTYPE GetOutputFilterParameters(IXAudio2Voice*, void*) = 0;  // 11
		virtual HRESULT STDMETHODCALLTYPE SetVolume(float, UINT32) = 0;                    // 12
		virtual void STDMETHODCALLTYPE GetVolume(float*) = 0;                              // 13
		virtual HRESULT STDMETHODCALLTYPE SetChannelVolumes(UINT32, const float*, UINT32) = 0;  // 14
		virtual void STDMETHODCALLTYPE GetChannelVolumes(UINT32, float*) = 0;              // 15
		virtual HRESULT STDMETHODCALLTYPE SetOutputMatrix(IXAudio2Voice*, UINT32, UINT32, const float*, UINT32) = 0;  // 16
		virtual void STDMETHODCALLTYPE GetOutputMatrix(IXAudio2Voice*, UINT32, UINT32, float*) = 0;  // 17
		virtual void STDMETHODCALLTYPE DestroyVoice() = 0;                                 // 18
	};

	struct IXAudio2 : IUnknown
	{
		virtual HRESULT STDMETHODCALLTYPE GetDeviceCount(UINT32* a_count) = 0;                                   // 3
		virtual HRESULT STDMETHODCALLTYPE GetDeviceDetails(UINT32 a_index, DeviceDetails* a_details) = 0;       // 4
		virtual HRESULT STDMETHODCALLTYPE Initialize(UINT32 a_flags, UINT32 a_processor) = 0;                    // 5
		virtual HRESULT STDMETHODCALLTYPE RegisterForCallbacks(IXAudio2EngineCallback* a_callback) = 0;          // 6
		virtual void STDMETHODCALLTYPE UnregisterForCallbacks(IXAudio2EngineCallback* a_callback) = 0;           // 7
		virtual HRESULT STDMETHODCALLTYPE CreateSourceVoice(IXAudio2Voice**, const WAVEFORMATEX*, UINT32, float, void*, const VoiceSends*, void*) = 0;  // 8
		virtual HRESULT STDMETHODCALLTYPE CreateSubmixVoice(IXAudio2Voice**, UINT32, UINT32, UINT32, UINT32, const VoiceSends*, void*) = 0;  // 9
		virtual HRESULT STDMETHODCALLTYPE CreateMasteringVoice(IXAudio2Voice**, UINT32, UINT32, UINT32, UINT32, void*) = 0;  // 10
		virtual HRESULT STDMETHODCALLTYPE StartEngine() = 0;                                                     // 11
		virtual void STDMETHODCALLTYPE StopEngine() = 0;                                                         // 12
	};

	namespace slot
	{
		inline constexpr std::size_t kRelease = 2;
		inline constexpr std::size_t kGetDeviceCount = 3;
		inline constexpr std::size_t kInitialize = 5;
		inline constexpr std::size_t kCreateSourceVoice = 8;
		inline constexpr std::size_t kCreateSubmixVoice = 9;
		inline constexpr std::size_t kCreateMasteringVoice = 10;
		inline constexpr std::size_t kStartEngine = 11;
		inline constexpr std::size_t kStopEngine = 12;
		inline constexpr std::size_t kDestroyVoice = 18;
	}

	using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IXAudio2*);
	using GetDeviceCountFn = HRESULT(STDMETHODCALLTYPE*)(IXAudio2*, UINT32*);
	using InitializeFn = HRESULT(STDMETHODCALLTYPE*)(IXAudio2*, UINT32, UINT32);
	using CreateSourceFn = HRESULT(STDMETHODCALLTYPE*)(IXAudio2*, IXAudio2Voice**, const WAVEFORMATEX*, UINT32, float, void*, const VoiceSends*, void*);
	using CreateSubmixFn = HRESULT(STDMETHODCALLTYPE*)(IXAudio2*, IXAudio2Voice**, UINT32, UINT32, UINT32, UINT32, const VoiceSends*, void*);
	using CreateMasteringFn = HRESULT(STDMETHODCALLTYPE*)(IXAudio2*, IXAudio2Voice**, UINT32, UINT32, UINT32, UINT32, void*);
	using StartEngineFn = HRESULT(STDMETHODCALLTYPE*)(IXAudio2*);
	using StopEngineFn = void(STDMETHODCALLTYPE*)(IXAudio2*);
	using DestroyVoiceFn = void(STDMETHODCALLTYPE*)(IXAudio2Voice*);
}
