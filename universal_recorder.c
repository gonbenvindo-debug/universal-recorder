#include "wcap.h"
#include "wcap_config.h"
#include "wcap_audio_capture.h"
#include "wcap_screen_capture.h"
#include "wcap_encoder.h"

#include <commctrl.h>
#pragma comment(lib, "comctl32")
#include <dxgi1_6.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <windowsx.h>

#pragma comment (lib, "ntdll")
#pragma comment (lib, "kernel32")
#pragma comment (lib, "user32")
#pragma comment (lib, "gdi32")
#pragma comment (lib, "msimg32")
#pragma comment (lib, "dxgi")
#pragma comment (lib, "d3dcompiler")
#pragma comment (lib, "d3d11")
#pragma comment (lib, "dwmapi")
#pragma comment (lib, "shell32")
#pragma comment (lib, "shlwapi")
#pragma comment (lib, "mfplat")
#pragma comment (lib, "mfuuid")
#pragma comment (lib, "mfreadwrite")
#pragma comment (lib, "evr")
#pragma comment (lib, "strmiids")
#pragma comment (lib, "ksuser")
#pragma comment (lib, "mmdevapi")
#pragma comment (lib, "ole32")
#pragma comment (lib, "wmcodecdspuuid")
#pragma comment (lib, "avrt")
#pragma comment (lib, "uxtheme")
#pragma comment (lib, "OneCore")
#pragma comment (lib, "CoreMessaging")

#if defined(_M_AMD64)
// this is needed to be able to use Nvidia Media Foundation encoders on Optimus systems
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

#define WM_WCAP_ALREADY_RUNNING (WM_USER+1)
#define WM_WCAP_STOP_CAPTURE    (WM_USER+2)
#define WM_WCAP_TRAY_TITLE      (WM_USER+3)
#define WM_WCAP_COMMAND         (WM_USER+4)

#define WCAP_AUDIO_CAPTURE_TIMER    1
#define WCAP_AUDIO_CAPTURE_INTERVAL 100 // msec

#define WCAP_VIDEO_UPDATE_TIMER     2
#define WCAP_VIDEO_UPDATE_INTERVAL  100 // msec

enum { CMD_TOGGLE = 10, CMD_FOLDER, CMD_SHORTCUT, CMD_QUIT, CMD_TIME };
#define HOT_RECORD_MONITOR 2
static HWND gDot;
static HMENU gMenu;
static BOOL gDialogOpen;
static ULONGLONG gStarted;
static DWORD gSeconds;
// constants
static WCHAR gConfigPath[MAX_PATH];
static LARGE_INTEGER gTickFreq;
static HICON gIdleIcon, gActiveIcon;

static UINT WM_TASKBARCREATED;
// recording state
static BOOL gRecordingStarted;
static BOOL gRecording;
static HMONITOR gRecordingMonitor;
static DWORD gRecordingLimitFramerate;

static UINT64 gRecordingLastFrame;
static UINT64 gRecordingNextEncode;

static EXECUTION_STATE gRecordingState;
static WCHAR gRecordingPath[MAX_PATH];

// globals
static HWND gWindow;
static Config gConfig;
static AudioCapture gAudio;
static BOOL gHasPlayback;
static LONGLONG gNextAudioFrame;
static ScreenCapture gCapture;
static Encoder gEncoder;

static void ShowNotification(LPCWSTR Message, LPCWSTR Title, DWORD Flags)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_INFO | NIF_TIP,
		.dwInfoFlags = Flags, // NIIF_INFO, NIIF_WARNING, NIIF_ERROR
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	StrCpyNW(Data.szInfo, Message, _countof(Data.szInfo));
	StrCpyNW(Data.szInfoTitle, Title ? Title : WCAP_TITLE, _countof(Data.szInfoTitle));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayTitle(LPCWSTR Title)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_TIP,
	};
	StrCpyNW(Data.szTip, Title, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void SetTrayIcon(HWND Window, DWORD action)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
		.uCallbackMessage = WM_WCAP_COMMAND,
		.hIcon = gRecording ? gActiveIcon : gIdleIcon,
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	SendMessageW(Window, WM_SETICON, ICON_SMALL, (LPARAM)Data.hIcon);
	Shell_NotifyIconW(action, &Data);
}

static void RemoveTrayIcon(HWND Window)
{
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
	};
	Shell_NotifyIconW(NIM_DELETE, &Data);
}

static ID3D11Device* CreateDevice(void);
#include "wcap_video_modes.h"

static void StartRecording(ID3D11Device* Device, VideoSize* size, UINT fpsIndex)
{
	SYSTEMTIME Time;
	GetLocalTime(&Time);

	int Error = SHCreateDirectoryExW(NULL, gConfig.OutputFolder, NULL);
	if (Error != ERROR_SUCCESS && Error != ERROR_FILE_EXISTS && Error != ERROR_ALREADY_EXISTS)
	{
		ShowNotification(L"Could not create the recordings folder.", L"Could not start recording", NIIF_WARNING);
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	WCHAR Filename[256];
	StrFormat(Filename, L"%04u%02u%02u_%02u%02u%02u_%03u.mp4", Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds);

	StrCpyW(gRecordingPath, gConfig.OutputFolder);
	if (!PathAppendW(gRecordingPath, Filename) || PathFileExistsW(gRecordingPath))
	{
		ShowNotification(L"Choose a different folder to save the recording.", WCAP_TITLE, NIIF_ERROR);
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	DXGI_RATIONAL rate = VideoRate(fpsIndex);
	gRecordingLimitFramerate = gFps[fpsIndex];
	Config settings = gConfig;
	settings.VideoMaxWidth = size->width;
	settings.VideoMaxHeight = size->height;
	settings.HardwareEncoder = !!(size->hardware & (1u << fpsIndex));

	EncoderConfig EncConfig =
	{
		.Width = gCapture.Rect.right - gCapture.Rect.left,
		.Height = gCapture.Rect.bottom - gCapture.Rect.top,
		.FramerateNum = rate.Numerator,
		.FramerateDen = rate.Denominator,
		.Config = &settings,
	};

	WAVEFORMATEX silenceFormat = { WAVE_FORMAT_PCM, 1, MIC_RATE, MIC_RATE * 2, 2, 16, 0 };
	if (gConfig.CaptureAudio)
	{
		gHasPlayback = AudioCapture_Start(&gAudio, NULL);
		BOOL hasMic = SUCCEEDED(Microphone_Start());
		if (!hasMic && !gHasPlayback)
		{
			ShowNotification(L"No microphone or system audio is available.", WCAP_TITLE, NIIF_WARNING);
			ScreenCapture_Stop(&gCapture);
			ID3D11Device_Release(Device);
			return;
		}
		if (!hasMic) ShowNotification(L"Microphone unavailable. Recording system audio only.", WCAP_TITLE, NIIF_WARNING);
		if (!gHasPlayback) ShowNotification(L"System audio unavailable. Recording the microphone only.", WCAP_TITLE, NIIF_WARNING);
		EncConfig.AudioFormat = gHasPlayback ? gAudio.Format : &silenceFormat;
		gNextAudioFrame = 0;
	}

	if (!Encoder_Start(&gEncoder, Device, gRecordingPath, &EncConfig))
	{
		if (gHasPlayback) AudioCapture_Stop(&gAudio);
		Microphone_Stop();
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	
	gRecordingNextEncode = 0;
	gRecordingLastFrame = 0;
	
	ScreenCapture_Start(&gCapture, gConfig.MouseCursor, gConfig.ShowRecordingBorder, gConfig.IncludeSecondaryWindows);

	if (gConfig.CaptureAudio)
	{
		SetTimer(gWindow, WCAP_AUDIO_CAPTURE_TIMER, WCAP_AUDIO_CAPTURE_INTERVAL, NULL);
	}
	SetTimer(gWindow, WCAP_VIDEO_UPDATE_TIMER, WCAP_VIDEO_UPDATE_INTERVAL, NULL);

	
	gRecordingState = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
	gRecording = TRUE;
	SetTrayIcon(gWindow, NIM_MODIFY);
	gStarted = GetTickCount64();
	ShowWindow(gDot, SW_SHOWNOACTIVATE);
	PostMessageW(gWindow, WM_WCAP_TRAY_TITLE, 0, 0);

	ID3D11Device_Release(Device);
}

static void EncodeAudioFrames(const void* samples, DWORD count)
{
	UINT64 time = gEncoder.StartTime + MFllMulDiv(gNextAudioFrame, gTickFreq.QuadPart, gEncoder.AudioSampleRate, 0);
	Encoder_NewSamples(&gEncoder, samples, count, time, gTickFreq.QuadPart);
	gNextAudioFrame += count;
}

static void EncodeSilenceUntil(LONGLONG frame)
{
	if (frame <= gNextAudioFrame) return;
	BYTE silence[4096];
	FillMemory(silence, sizeof(silence), gHasPlayback && gAudio.Format->wBitsPerSample == 8 ? 128 : 0);
	while (frame > gNextAudioFrame)
	{
		DWORD count = (DWORD)min(frame - gNextAudioFrame, (LONGLONG)(sizeof(silence) / gEncoder.AudioFrameSize));
		EncodeAudioFrames(silence, count);
	}
}

static void EncodeCapturedAudio(BOOL finish)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (gMic.Source && FAILED(Microphone_Read(MFllMulDiv(now.QuadPart, MF_UNITS_PER_SECOND, gTickFreq.QuadPart, 0))))
	{
		Microphone_Stop();
		ShowNotification(L"The microphone stopped sending audio.", WCAP_TITLE, NIIF_WARNING);
	}
	if (!gEncoder.StartTime) return;

	// Allow time for the Windows speech filter to return audio for the same video interval.
	LONGLONG cutoff = now.QuadPart - (!finish ? gTickFreq.QuadPart / 8 : 0);
	if (cutoff <= (LONGLONG)gEncoder.StartTime) return;
	LONGLONG end = MFllMulDiv(cutoff - gEncoder.StartTime, gEncoder.AudioSampleRate, gTickFreq.QuadPart, 0);
	AudioCaptureData packet;
	while (gHasPlayback && AudioCapture_GetData(&gAudio, &packet, now.QuadPart))
	{
		LONGLONG start = MFllMulDiv((LONGLONG)packet.Time - (LONGLONG)gEncoder.StartTime,
			gEncoder.AudioSampleRate, gTickFreq.QuadPart, 0);
		if (start >= end) break;
		EncodeSilenceUntil(start);
		LONGLONG skip = gNextAudioFrame - start;
		if (skip < (LONGLONG)packet.Count)
		{
			DWORD count = (DWORD)min((LONGLONG)packet.Count - skip, end - gNextAudioFrame);
			if (count) EncodeAudioFrames((BYTE*)packet.Samples + skip * gEncoder.AudioFrameSize, count);
			if (skip + count < (LONGLONG)packet.Count) break;
		}
		AudioCapture_ReleaseData(&gAudio, &packet);
	}
	// Loopback devices can stop delivering packets while the speakers are silent.
	EncodeSilenceUntil(end);
}

static void StopRecording(void)
{
	gRecording = FALSE;
	SetTrayIcon(gWindow, NIM_MODIFY);
	ShowWindow(gDot, SW_HIDE);
	gSeconds = (DWORD)((GetTickCount64() - gStarted) / 1000);
	SetThreadExecutionState(gRecordingState);

	if (gConfig.CaptureAudio)
	{
		KillTimer(gWindow, WCAP_AUDIO_CAPTURE_TIMER);
		if (gHasPlayback) AudioCapture_Flush(&gAudio);
		EncodeCapturedAudio(TRUE);
		if (gHasPlayback) AudioCapture_Stop(&gAudio);
	}
	KillTimer(gWindow, WCAP_VIDEO_UPDATE_TIMER);

	ScreenCapture_Stop(&gCapture);
	if (FAILED(Encoder_Stop(&gEncoder)))
		ShowNotification(L"Windows could not finish writing the MP4 file.", WCAP_TITLE, NIIF_ERROR);
	Microphone_Stop();
	UpdateTrayTitle(WCAP_TITLE);
}

static ID3D11Device* CreateDevice(void)
{
	IDXGIAdapter* Adapter = NULL;

	if (gConfig.HardwareEncoder)
	{
		IDXGIFactory* Factory;
		if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void**)&Factory)))
		{
			IDXGIFactory6* Factory6;
			if (SUCCEEDED(IDXGIFactory_QueryInterface(Factory, &IID_IDXGIFactory6, (void**)&Factory6)))
			{
				DXGI_GPU_PREFERENCE Preference = gConfig.HardwarePreferIntegrated ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
				if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(Factory6, 0, Preference, &IID_IDXGIAdapter, &Adapter)))
				{
					// just to be safe
					Adapter = NULL;
				}
				IDXGIFactory6_Release(Factory6);
			}
			IDXGIFactory_Release(Factory);
		}
	}

	ID3D11Device* Device;

	UINT flags = 0;
#ifndef NDEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	// if adapter is selected then driver type must be unknown
	D3D_DRIVER_TYPE Driver = Adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
	if (FAILED(D3D11CreateDevice(Adapter, Driver, NULL, flags, (D3D_FEATURE_LEVEL[]) { D3D_FEATURE_LEVEL_11_0 }, 1, D3D11_SDK_VERSION, &Device, NULL, NULL)))
	{
		ShowNotification(L"Could not create the Direct3D 11 device.", L"Error", NIIF_ERROR);
		Device = NULL;
	}
	if (Adapter)
	{
		IDXGIAdapter_Release(Adapter);
	}

	if (flags & D3D11_CREATE_DEVICE_DEBUG)
	{
		ID3D11InfoQueue* Info;
		if (SUCCEEDED(ID3D11Device_QueryInterface(Device, &IID_ID3D11InfoQueue, &Info)))
		{
			ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
			ID3D11InfoQueue_Release(Info);
		}
	}

	return Device;
}

static void CaptureMonitor(HMONITOR Monitor)
{
	if (!Monitor) {
		POINT pos;
		GetCursorPos(&pos);
		Monitor = MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST);
	}
	UINT fpsIndex = 0;
	RefreshMonitors();
	RecorderMonitor* target = SelectedMonitor(Monitor);
	VideoSize* size = LoadVideoModes(target) ? SelectedVideoMode(&fpsIndex) : NULL;
	if (!size) {
		ShowNotification(L"No supported H.264 format is available for this monitor.", WCAP_TITLE, NIIF_WARNING);
		return;
	}
	Monitor = target->handle;
	MONITORINFO Info = { .cbSize = sizeof(Info) };
	if (!GetMonitorInfoW(Monitor, &Info)) return;
	SetWindowPos(gDot, HWND_TOPMOST, Info.rcMonitor.right - 28, Info.rcMonitor.top + 14,
		14, 14, SWP_NOACTIVATE);
	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		return;
	}

	if (!ScreenCapture_CreateForMonitor(&gCapture, Device, Monitor, NULL))
	{
		ID3D11Device_Release(Device);
		ShowNotification(L"Could not record the selected monitor.", L"Error", NIIF_WARNING);
		return;
	}

	gRecordingMonitor = Monitor;
	StartRecording(Device, size, fpsIndex);
}


static BOOL RegisterShortcut(DWORD key)
{
	return RegisterHotKey(gWindow, HOT_RECORD_MONITOR, HOT_GET_MOD(key) | MOD_NOREPEAT, HOT_GET_KEY(key));
}

static INT_PTR CALLBACK ShortcutProc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_INITDIALOG)
	{
		DWORD mod = HOT_GET_MOD(gConfig.ShortcutMonitor);
		WORD flags = ((mod & MOD_CONTROL) ? HOTKEYF_CONTROL : 0) |
			((mod & MOD_SHIFT) ? HOTKEYF_SHIFT : 0) | ((mod & MOD_ALT) ? HOTKEYF_ALT : 0);
		SendDlgItemMessageW(window, 101, HKM_SETHOTKEY, MAKEWORD(HOT_GET_KEY(gConfig.ShortcutMonitor), flags), 0);
		return TRUE;
	}
	if (msg != WM_COMMAND) return FALSE;
	if (LOWORD(wp) == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
	if (LOWORD(wp) != IDOK) return FALSE;
	WORD value = (WORD)SendDlgItemMessageW(window, 101, HKM_GETHOTKEY, 0, 0);
	WORD flags = HIBYTE(value);
	DWORD mod = ((flags & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) |
		((flags & HOTKEYF_SHIFT) ? MOD_SHIFT : 0) | ((flags & HOTKEYF_ALT) ? MOD_ALT : 0);
	if (!LOBYTE(value)) return TRUE;
	DWORD key = HOT_KEY(LOBYTE(value), mod);
	UnregisterHotKey(gWindow, HOT_RECORD_MONITOR);
	if (!RegisterShortcut(key))
	{
		RegisterShortcut(gConfig.ShortcutMonitor);
		MessageBoxW(window, L"That shortcut is already in use. Choose a different key combination.", WCAP_TITLE, MB_ICONWARNING);
		return TRUE;
	}
	gConfig.ShortcutMonitor = key;
	if (!Config__WriteInt(gConfigPath, L"ShortcutMonitor", key))
		MessageBoxW(window, L"This shortcut could not be saved. It will work for this session only.", WCAP_TITLE, MB_ICONWARNING);
	EndDialog(window, IDOK);
	return TRUE;
}

static void ChooseFolder(HWND window)
{
	IFileDialog* dialog;
	if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, &dialog))) return;
	IFileDialog_SetOptions(dialog, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	IFileDialog_SetTitle(dialog, L"Recordings folder");
	IShellItem* folder;
	if (SUCCEEDED(SHCreateItemFromParsingName(gConfig.OutputFolder, NULL, &IID_IShellItem, &folder)))
	{
		IFileDialog_SetFolder(dialog, folder);
		IShellItem_Release(folder);
	}
	if (SUCCEEDED(IFileDialog_Show(dialog, window)) && SUCCEEDED(IFileDialog_GetResult(dialog, &folder)))
	{
		PWSTR path;
		if (SUCCEEDED(IShellItem_GetDisplayName(folder, SIGDN_FILESYSPATH, &path)))
		{
			if (lstrlenW(path) < MAX_PATH - 32 && WritePrivateProfileStringW(INI_SECTION, L"OutputFolder", path, gConfigPath))
				StrCpyW(gConfig.OutputFolder, path);
			else
				MessageBoxW(window, L"Could not save the selected folder.", WCAP_TITLE, MB_ICONWARNING);
			CoTaskMemFree(path);
		}
		IShellItem_Release(folder);
	}
	IFileDialog_Release(dialog);
}

static void UpdateDuration(void)
{
	DWORD seconds = gRecording ? (DWORD)((GetTickCount64() - gStarted) / 1000) : gSeconds;
	WCHAR label[96];
	StrFormat(label, L"%ls  %02u:%02u:%02u", gRecording ? L"Recording" : L"Last recording",
		seconds / 3600, seconds / 60 % 60, seconds % 60);
	if (gMenu) ModifyMenuW(gMenu, CMD_TIME, MF_BYCOMMAND | MF_GRAYED, CMD_TIME, label);
	UpdateTrayTitle(label);
}

static LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	case WM_DISPLAYCHANGE:
	case WM_DEVICECHANGE:
		gVideo.monitor = NULL;
		if (gMenu) EndMenu();
		return 0;
	case WM_CREATE: SetTrayIcon(window, NIM_ADD); return 0;
	case WM_ENDSESSION:
		if (!wp) return 0;
		// fall through
	case WM_DESTROY:
		if (gRecording) StopRecording();
		RemoveTrayIcon(window);
		PostQuitMessage(0);
		return 0;
	case WM_TIMER:
		if (!gRecording) return 0;
		if (wp == WCAP_AUDIO_CAPTURE_TIMER) EncodeCapturedAudio(FALSE);
		if (wp == WCAP_VIDEO_UPDATE_TIMER)
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			Encoder_Update(&gEncoder, now.QuadPart, gTickFreq.QuadPart);
			DWORD seconds = (DWORD)((GetTickCount64() - gStarted) / 1000);
			if (seconds != gSeconds) { gSeconds = seconds; UpdateDuration(); }
		}
		return 0;
	case WM_WCAP_STOP_CAPTURE:
		if (gRecording) StopRecording();
		return 0;
	case WM_WCAP_TRAY_TITLE: UpdateDuration(); return 0;
	case WM_WCAP_COMMAND:
		if (LOWORD(lp) != WM_RBUTTONUP && LOWORD(lp) != WM_CONTEXTMENU) return 0;
		// fall through
	case WM_CONTEXTMENU:
	{
		if (gRecordingStarted || gDialogOpen || gMenu) return 0;
		POINT pos;
		GetCursorPos(&pos);
		HMONITOR monitor = MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST);
		gMenu = CreatePopupMenu();
		if (gStarted)
		{
			AppendMenuW(gMenu, MF_GRAYED, CMD_TIME, L"");
			UpdateDuration();
		}
		AppendMenuW(gMenu, MF_STRING, CMD_TOGGLE, gRecording ? L"Stop recording" : L"Start recording");
		AppendMenuW(gMenu, MF_SEPARATOR, 0, NULL);
		RefreshMonitors();
		MonitorMenu(gMenu);
		VideoMenus(gMenu, SelectedMonitor(gRecording ? gRecordingMonitor : monitor));
		AppendMenuW(gMenu, MF_STRING, CMD_FOLDER, L"Choose recordings folder...");
		AppendMenuW(gMenu, MF_STRING, CMD_SHORTCUT, L"Set keyboard shortcut...");
		AppendMenuW(gMenu, MF_STRING, CMD_QUIT, L"Exit");
		SetForegroundWindow(window);
		UINT command = TrackPopupMenu(gMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
			pos.x, pos.y, 0, window, NULL);
		DestroyMenu(gMenu);
		gMenu = NULL;
		if (command) SendMessageW(window, WM_COMMAND, command, (LPARAM)monitor);
		return 0;
	}
	case WM_HOTKEY:
		if (wp != HOT_RECORD_MONITOR) return 0;
		wp = CMD_TOGGLE;
		lp = 0;
		// fall through
	case WM_COMMAND:
		if (gDialogOpen || gRecordingStarted) return 0;
		if (wp == CMD_TOGGLE)
		{
			if (gMenu) EndMenu();
			gRecordingStarted = TRUE;
			if (gRecording) StopRecording(); else CaptureMonitor((HMONITOR)lp);
			gRecordingStarted = FALSE;
		}
		else if (wp == CMD_FOLDER || wp == CMD_SHORTCUT)
		{
			gDialogOpen = TRUE;
			if (wp == CMD_FOLDER) ChooseFolder(window);
			else DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(3), window, ShortcutProc);
			gDialogOpen = FALSE;
		}
		else if (wp == CMD_QUIT) DestroyWindow(window);
		else if (wp >= CMD_MONITOR_BASE && !gRecording && gMonitors &&
			wp - CMD_MONITOR_BASE <= (UINT)DSA_GetItemCount(gMonitors))
		{
			RecorderMonitor* target = wp == CMD_MONITOR_BASE ? NULL : DSA_GetItemPtr(gMonitors, (int)(wp - CMD_MONITOR_BASE - 1));
			StrCpyW(gConfig.MonitorPath, target ? target->path : L"");
			if (!WritePrivateProfileStringW(INI_SECTION, L"Monitor", gConfig.MonitorPath, gConfigPath))
				ShowNotification(L"This setting could not be saved. It will apply to this session only.", WCAP_TITLE, NIIF_WARNING);
		}
		else if (wp >= CMD_FPS_BASE && !gRecording && gVideo.monitor)
		{
			UINT rate = 0;
			VideoSize* selected = SelectedVideoMode(&rate);
			BOOL saved = TRUE;
			if (!selected) return 0;
			if (wp >= CMD_SIZE_BASE && wp - CMD_SIZE_BASE < (UINT)DSA_GetItemCount(gVideo.sizes)) {
				VideoSize* size = DSA_GetItemPtr(gVideo.sizes, (int)(wp - CMD_SIZE_BASE));
				if (!(size->supported & (1u << rate))) return 0;
				gConfig.VideoMaxWidth = size->width;
				gConfig.VideoMaxHeight = size->height;
				saved = Config__WriteInt(gConfigPath, L"VideoSize", MAKELONG(size->height, size->width));
			} else if (wp - CMD_FPS_BASE < _countof(gFps)) {
				UINT index = (UINT)(wp - CMD_FPS_BASE);
				if (!(selected->supported & (1u << index))) return 0;
				gConfig.VideoMaxFramerate = gFps[index];
				saved = Config__WriteInt(gConfigPath, L"VideoFPS", gFps[index]);
			}
			if (!saved) ShowNotification(L"This setting could not be saved. It will apply to this session only.", WCAP_TITLE, NIIF_WARNING);
		}
		return 0;
	}
	if (msg == WM_TASKBARCREATED) { SetTrayIcon(window, NIM_ADD); if (gRecording) UpdateDuration(); return 0; }
	return DefWindowProcW(window, msg, wp, lp);
}

static bool OnCaptureFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame)
{
	if (Frame == NULL)
	{
		PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
		return true;
	}

	BOOL DoEncode = TRUE;
	DWORD LimitFramerate = gRecordingLimitFramerate;
	if (LimitFramerate != 0)
	{
		if (Frame->Time * LimitFramerate < gRecordingNextEncode)
		{
			DoEncode = FALSE;
		}
		else
		{
			if (gRecordingNextEncode == 0)
			{
				gRecordingNextEncode = Frame->Time * LimitFramerate;
			}
			UINT64 elapsed = Frame->Time * LimitFramerate - gRecordingNextEncode;
			gRecordingNextEncode += (1 + elapsed / gTickFreq.QuadPart) * gTickFreq.QuadPart;
		}
	}

	// ignore frames if it comes from the past
	if (Frame->Time <= gRecordingLastFrame)
	{
		DoEncode = FALSE;
	}
	gRecordingLastFrame = Frame->Time;

	if (DoEncode)
	{
		Encoder_NewFrame(&gEncoder, Frame->Texture, Frame->Rect, Frame->Time, gTickFreq.QuadPart);
	}

	return true;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int cmdshow)
{
	SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, L"en-US\0", NULL);
	WNDCLASSEXW WindowClass =
	{
		.cbSize = sizeof(WindowClass),
		.lpfnWndProc = WindowProc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = L"UniversalRecorderTray",
	};

	HWND Existing = FindWindowW(WindowClass.lpszClassName, NULL);
	if (Existing)
	{
		PostMessageW(Existing, WM_WCAP_ALREADY_RUNNING, 0, 0);
		ExitProcess(0);
	}

	if (!ScreenCapture_CanHideMouseCursor())
	{
		MessageBoxW(NULL, L"Universal Recorder requires Windows 10 version 2004 or later.", WCAP_TITLE, MB_ICONEXCLAMATION);
		ExitProcess(0);
	}

	SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, gConfigPath);
	PathAppendW(gConfigPath, L"Universal Recorder.ini");

	HR(CoInitializeEx(0, COINIT_APARTMENTTHREADED));

	Config_Defaults(&gConfig);
	Config_Load(&gConfig, gConfigPath);
	ScreenCapture_Create(&gCapture, &OnCaptureFrame, false);
	Encoder_Init(&gEncoder);

	QueryPerformanceFrequency(&gTickFreq);

	WNDCLASSW dotClass = { .lpfnWndProc = DefWindowProcW, .hInstance = WindowClass.hInstance,
		.lpszClassName = L"UniversalRecorderDot", .hbrBackground = CreateSolidBrush(RGB(239, 35, 44)) };
	RegisterClassW(&dotClass);
	gDot = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
		dotClass.lpszClassName, NULL, WS_POPUP | WS_DISABLED, 0, 0, 14, 14, NULL, NULL, dotClass.hInstance, NULL);
	if (!gDot || !SetWindowRgn(gDot, CreateEllipticRgn(0, 0, 14, 14), FALSE) ||
		!SetWindowDisplayAffinity(gDot, WDA_EXCLUDEFROMCAPTURE))
	{
		MessageBoxW(NULL, L"Could not hide the recording indicator from the video.", WCAP_TITLE, MB_ICONERROR);
		ExitProcess(1);
	}
	INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_HOTKEY_CLASS };
	InitCommonControlsEx(&controls);
	gIdleIcon = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(1));
	gActiveIcon = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(2));
	
	Assert(gIdleIcon && gActiveIcon);

	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	gWindow = CreateWindowExW(
		0, WindowClass.lpszClassName, WCAP_TITLE, WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, WindowClass.hInstance, NULL);
	if (!gWindow)
	{
		ExitProcess(0);
	}
	if (!RegisterShortcut(gConfig.ShortcutMonitor))
	{
		MessageBoxW(NULL,
			L"The recording shortcut is already in use. Choose another shortcut from the tray menu.",
			WCAP_TITLE, MB_ICONEXCLAMATION);
	}

	for (;;)
	{
		MSG Message;
		BOOL Result = GetMessageW(&Message, NULL, 0, 0);
		if (Result == 0)
		{
			ExitProcess(0);
		}
		Assert(Result > 0);

		TranslateMessage(&Message);
		DispatchMessageW(&Message);
	}
}

#ifdef NDEBUG
// The recorder uses only Win32 startup services and initializes its own /GS cookie.
extern void __cdecl __security_init_cookie(void);
__declspec(safebuffers) void UniversalRecorderEntry(void)
{
	__security_init_cookie();
	ExitProcess(WinMain(GetModuleHandleW(NULL), NULL, NULL, SW_HIDE));
}
#endif
