#pragma once

#include "wcap.h"

//
// interface
//

#define CONFIG_VIDEO_H264 0
#define CONFIG_VIDEO_H265 1
#define CONFIG_VIDEO_AV1  2

#define CONFIG_VIDEO_BASE    0
#define CONFIG_VIDEO_MAIN    1
#define CONFIG_VIDEO_HIGH    2
#define CONFIG_VIDEO_MAIN_10 3

#define CONFIG_AUDIO_AAC  0
#define CONFIG_AUDIO_FLAC 1

typedef struct
{
	// capture
	BOOL MouseCursor;
	BOOL OnlyClientArea;
	BOOL ShowRecordingBorder;
	BOOL KeepRoundedWindowCorners;
	BOOL IncludeSecondaryWindows;
	BOOL HardwareEncoder;
	BOOL HardwarePreferIntegrated;
	// output
	WCHAR OutputFolder[MAX_PATH];
	WCHAR MonitorPath[256];
	BOOL OpenFolder;
	BOOL FragmentedOutput;
	BOOL EnableLimitLength;
	BOOL EnableLimitSize;
	DWORD LimitLength;
	DWORD LimitSize;
	// video
	BOOL GammaCorrectResize;
	BOOL ImprovedColorConversion;
	DWORD VideoCodec;
	DWORD VideoProfile;
	DWORD VideoMaxWidth;
	DWORD VideoMaxHeight;
	DWORD VideoMaxFramerate;
	// audio
	BOOL CaptureAudio;
	BOOL ApplicationLocalAudio;
	DWORD AudioCodec;
	DWORD AudioChannels;
	DWORD AudioSamplerate;
	DWORD AudioBitrate;
	// shortcuts
	DWORD ShortcutMonitor;
	DWORD ShortcutWindow;
	DWORD ShortcutRegion;
}
Config;

#define HOT_KEY(Key, Mod) ((Key) | ((Mod) << 24))
#define HOT_GET_KEY(KeyMod) ((KeyMod) & 0xffffff)
#define HOT_GET_MOD(KeyMod) (((KeyMod) >> 24) & 0xff)

static void Config_Defaults(Config* C);
static void Config_Load(Config* C, LPCWSTR FileName);



//
// implementation
//

#include "wcap_screen_capture.h"
#include "wcap_audio_capture.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <knownfolders.h>
#include <windowsx.h>

#define INI_SECTION WCAP_TITLE

void Config_Defaults(Config* C)
{
	*C = (Config)
	{
		// capture
		.MouseCursor = TRUE,
		.OnlyClientArea = TRUE,
		.ShowRecordingBorder = FALSE,
		.KeepRoundedWindowCorners = TRUE,
		.IncludeSecondaryWindows = FALSE,
		.HardwareEncoder = TRUE,
		.HardwarePreferIntegrated = TRUE,
		// output
		.OpenFolder = FALSE,
		.FragmentedOutput = FALSE,
		.EnableLimitLength = FALSE,
		.EnableLimitSize = FALSE,
		.LimitLength = 60,
		.LimitSize = 25,
		// video
		.GammaCorrectResize = FALSE,
		.ImprovedColorConversion = FALSE,
		.VideoCodec = CONFIG_VIDEO_H264,
		.VideoProfile = CONFIG_VIDEO_HIGH,
		.VideoMaxWidth = 1920,
		.VideoMaxHeight = 1080,
		.VideoMaxFramerate = 24,
		// audio
		.CaptureAudio = TRUE,
		.ApplicationLocalAudio = TRUE,
		.AudioCodec = CONFIG_AUDIO_AAC,
		.AudioChannels = 2,
		.AudioSamplerate = 48000,
		.AudioBitrate = 96,
		// shortcuts
		.ShortcutMonitor = HOT_KEY('R', MOD_CONTROL | MOD_SHIFT),
		.ShortcutWindow = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL | MOD_WIN),
		.ShortcutRegion = HOT_KEY(VK_SNAPSHOT, MOD_CONTROL | MOD_SHIFT),
	};

	LPWSTR VideoFolder;
	HR(SHGetKnownFolderPath(&FOLDERID_Videos, KF_FLAG_DEFAULT, NULL, &VideoFolder));
	StrCpyNW(C->OutputFolder, VideoFolder, _countof(C->OutputFolder));
	CoTaskMemFree(VideoFolder);
}


static void Config_Load(Config* c, LPCWSTR file)
{
	GetPrivateProfileStringW(INI_SECTION, L"Monitor", L"", c->MonitorPath, _countof(c->MonitorPath), file);
	WCHAR folder[MAX_PATH];
	if (GetPrivateProfileStringW(INI_SECTION, L"OutputFolder", L"", folder, MAX_PATH, file))
		StrCpyW(c->OutputFolder, folder);
	DWORD size = GetPrivateProfileIntW(INI_SECTION, L"VideoSize", MAKELONG(c->VideoMaxHeight, c->VideoMaxWidth), file);
	if (HIWORD(size) && LOWORD(size)) {
		c->VideoMaxWidth = HIWORD(size);
		c->VideoMaxHeight = LOWORD(size);
	}
	c->VideoMaxFramerate = GetPrivateProfileIntW(INI_SECTION, L"VideoFPS", c->VideoMaxFramerate, file);
	c->ShortcutMonitor = GetPrivateProfileIntW(INI_SECTION, L"ShortcutMonitor", c->ShortcutMonitor, file);
}
static BOOL Config__WriteInt(LPCWSTR FileName, LPCWSTR Key, DWORD Value)
{
	WCHAR Text[64];
	StrFormat(Text, L"%u", Value);
	return WritePrivateProfileStringW(INI_SECTION, Key, Text, FileName);
}

