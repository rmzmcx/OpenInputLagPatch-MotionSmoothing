#pragma once

enum UiMessage {
	UiMessage_GamePathUnavailable,
	UiMessage_GameHashFailed,
	UiMessage_UnsupportedGame,
	UiMessage_NonOriginalGame,
	UiMessage_VpatchIncompatible,
	UiMessage_DebugWait,
	UiMessage_AskFullscreen,
	UiMessage_MissingPatchFunction,
	UiMessage_InvalidRefreshRate,
	UiMessage_WaitableTimerFailed,
	UiMessage_LimiterNotInitialized,
	UiMessage_TemporaryFontTextureCreateFailed,
	UiMessage_TemporaryFontTextureLockFailed,
	UiMessage_FontTextureCreateFailed,
	UiMessage_VertexBufferCreateFailed,
	UiMessage_IndexBufferCreateFailed,
	UiMessage_TooManyCharacters,
	UiMessage_DeviceResetFailed,
	UiMessage_TestCooperativeLevelFailed,
	UiMessage_CreateDeviceExFailed,
	UiMessage_CreateDeviceFailed,
	UiMessage_Direct3DCreate9ExNotFound,
	UiMessage_Direct3DCreate9ExFailed,
	UiMessage_Direct3DCreate9NotFound,
	UiMessage_Direct3DCreate9Failed,
	UiMessage_Count
};

bool sha256_file(wchar_t* path, char* hash_str);
const wchar_t* ui_message_text(UiMessage message);
int show_ui_message(UiMessage message, unsigned int type, ...);
void queue_ui_message(UiMessage message, unsigned int type, bool exit_after, const wchar_t* argument = nullptr);
void show_queued_ui_messages();
void __declspec(noreturn) panic_msgbox(UiMessage message, ...);

/// defer implementation for C++
/// http://www.gingerbill.org/article/defer-in-cpp.html
/// ----------------------------

// Fun fact: in the vast majority of cases the compiler
// optimizes this down to normal conditional branching.
template <typename F>
struct privDefer {
    F f;
    explicit privDefer(F f)
        : f(f)
    {
    }
    ~privDefer() { f(); }
};

template <typename F>
privDefer<F> defer_func(F f)
{
    return privDefer<F>(f);
}

#define DEFER_1(x, y) x##y
#define DEFER_2(x, y) DEFER_1(x, y)
#define DEFER_3(x) DEFER_2(x, __COUNTER__)
#define defer(code) auto DEFER_3(_defer_) = defer_func([&]() { code; })
