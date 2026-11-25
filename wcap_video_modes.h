#pragma once

// A zero preference tracks the exact refresh rate, including 59.94 Hz and other fractional modes.
static const UINT gFps[] = { 5, 10, 15, 20, 24, 25, 30, 48, 50, 60, 72, 75, 90, 100, 120,
	144, 165, 180, 200, 240, 300, 360, 480, 0 };
enum { CMD_FPS_BASE = 100, CMD_SIZE_BASE = 1000, CMD_MONITOR_BASE = 30000 };
typedef struct { DWORD width, height, supported, hardware; } VideoSize;
static struct {
	HMONITOR monitor;
	HDSA sizes;
	DWORD width, height;
	DXGI_RATIONAL refresh;
} gVideo;

typedef struct {
	HMONITOR handle;
	WCHAR path[256], name[128];
	DXGI_RATIONAL refresh;
} RecorderMonitor;

static HDSA gMonitors;
typedef struct {
	DISPLAYCONFIG_PATH_INFO* paths;
	DISPLAYCONFIG_MODE_INFO* modes;
	UINT32 pathCount, modeCount;
} DisplayPaths;

static BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC dc, RECT* rect, LPARAM data)
{
	MONITORINFOEXW info = { .cbSize = sizeof(info) };
	if (!GetMonitorInfoW(handle, (MONITORINFO*)&info)) return TRUE;
	DISPLAY_DEVICEW device = { .cb = sizeof(device) };
	EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME);
	RecorderMonitor base = { .handle = handle };
	StrCpyNW(base.name, device.DeviceString[0] ? device.DeviceString : info.szDevice, _countof(base.name));
	StrCpyNW(base.path, device.DeviceID[0] ? device.DeviceID : info.szDevice, _countof(base.path));
	DisplayPaths* config = (DisplayPaths*)data;
	int before = DSA_GetItemCount(gMonitors);
	for (UINT32 i = 0; i < config->pathCount; ++i) {
		DISPLAYCONFIG_PATH_INFO* path = &config->paths[i];
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source = { .header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
			sizeof(source), path->sourceInfo.adapterId, path->sourceInfo.id } };
		if (DisplayConfigGetDeviceInfo(&source.header) || lstrcmpiW(source.viewGdiDeviceName, info.szDevice)) continue;
		RecorderMonitor entry = base;
		DISPLAYCONFIG_TARGET_DEVICE_NAME target = { .header = { DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
			sizeof(target), path->targetInfo.adapterId, path->targetInfo.id } };
		if (!DisplayConfigGetDeviceInfo(&target.header)) {
			if (target.monitorFriendlyDeviceName[0]) StrCpyW(entry.name, target.monitorFriendlyDeviceName);
			if (target.monitorDevicePath[0]) StrCpyW(entry.path, target.monitorDevicePath);
		}
		DISPLAYCONFIG_RATIONAL rate = path->targetInfo.refreshRate;
		UINT32 index = path->targetInfo.modeInfoIdx;
		if ((!rate.Numerator || !rate.Denominator) && index < config->modeCount)
			rate = config->modes[index].targetMode.targetVideoSignalInfo.vSyncFreq;
		entry.refresh = (DXGI_RATIONAL){ rate.Numerator, rate.Denominator };
		DSA_AppendItem(gMonitors, &entry);
	}
	if (before == DSA_GetItemCount(gMonitors)) DSA_AppendItem(gMonitors, &base);
	return TRUE;
}

static void RefreshMonitors(void)
{
	if (!gMonitors) gMonitors = DSA_Create(sizeof(RecorderMonitor), 4);
	if (!gMonitors) return;
	DSA_DeleteAllItems(gMonitors);
	DisplayPaths config = { 0 };
	UINT32 paths, modes;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) == ERROR_SUCCESS) {
		config.paths = calloc(paths, sizeof(*config.paths));
		config.modes = calloc(modes, sizeof(*config.modes));
		if (config.paths && config.modes &&
			QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, config.paths, &modes, config.modes, NULL) == ERROR_SUCCESS) {
			config.pathCount = paths;
			config.modeCount = modes;
		}
	}
	EnumDisplayMonitors(NULL, NULL, CollectMonitor, (LPARAM)&config);
	free(config.paths);
	free(config.modes);
}

static RecorderMonitor* SelectedMonitor(HMONITOR automatic)
{
	RecorderMonitor* fallback = NULL;
	for (int i = 0; gMonitors && i < DSA_GetItemCount(gMonitors); ++i) {
		RecorderMonitor* item = DSA_GetItemPtr(gMonitors, i);
		if (!lstrcmpiW(item->path, gConfig.MonitorPath)) return item;
		if (!fallback || item->handle == automatic) fallback = item;
	}
	return fallback;
}

static void MonitorMenu(HMENU parent)
{
	if (!gMonitors || DSA_GetItemCount(gMonitors) < 2) return;
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, CMD_MONITOR_BASE, L"Automático (onde está o ponteiro)");
	UINT checked = CMD_MONITOR_BASE;
	for (int i = 0; i < DSA_GetItemCount(gMonitors); ++i) {
		RecorderMonitor* item = DSA_GetItemPtr(gMonitors, i);
		int same = 0, number = 0;
		for (int j = 0; j < DSA_GetItemCount(gMonitors); ++j) {
			RecorderMonitor* other = DSA_GetItemPtr(gMonitors, j);
			if (!lstrcmpiW(item->name, other->name)) { ++same; if (j <= i) ++number; }
		}
		WCHAR text[300];
		int length = 0;
		for (const WCHAR* p = item->name; *p; ++p) {
			if (*p == L'&') text[length++] = L'&';
			text[length++] = *p;
		}
		text[length] = 0;
		if (same > 1) _snwprintf(text + length, _countof(text) - length, L" (%d)", number);
		UINT id = CMD_MONITOR_BASE + 1 + i;
		if (!lstrcmpiW(item->path, gConfig.MonitorPath)) checked = id;
		AppendMenuW(menu, MF_STRING, id, text);
	}
	CheckMenuItem(menu, checked, MF_BYCOMMAND | MF_CHECKED);
	AppendMenuW(parent, MF_POPUP | (gRecording ? MF_GRAYED : 0), (UINT_PTR)menu, L"Monitor");
}

static DXGI_RATIONAL VideoRate(UINT index)
{
	return gFps[index] ? (DXGI_RATIONAL){ gFps[index], 1 } : gVideo.refresh;
}

static void AddVideoSize(DWORD width, DWORD height)
{
	if (width < 2 || height < 2 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
		height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return;
	VideoSize entry = { (width + 1) & ~1u, (height + 1) & ~1u };
	int count = DSA_GetItemCount(gVideo.sizes);
	int i = 0;
	for (; i < count; ++i) {
		VideoSize* item = DSA_GetItemPtr(gVideo.sizes, i);
		if (item->width == entry.width && item->height == entry.height) return;
		if (item->width > entry.width || (item->width == entry.width && item->height > entry.height)) break;
	}
	DSA_InsertItem(gVideo.sizes, i, &entry);
}

static void ProbeVideoEncoder(ID3D11Device* device, BOOL hardware)
{
	IMFActivate** list = NULL;
	UINT32 count = 0;
	if (FAILED(Encoder_Enumerate(device, hardware, &MFVideoFormat_NV12, &MFVideoFormat_H264, &list, &count))) return;
	for (UINT32 e = 0; e < count; ++e) {
		IMFTransform* codec;
		if (SUCCEEDED(IMFActivate_ActivateObject(list[e], &IID_IMFTransform, (void**)&codec))) {
			IMFAttributes* attributes;
			UINT32 capacity = 0;
			if (SUCCEEDED(IMFTransform_GetAttributes(codec, &attributes))) {
				if (hardware) IMFAttributes_SetUINT32(attributes, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
				IMFAttributes_GetUINT32(attributes, &MF_VIDEO_MAX_MB_PER_SEC, &capacity);
				capacity &= 0x0fffffff;
				IMFAttributes_Release(attributes);
			}
			for (int i = 0; i < DSA_GetItemCount(gVideo.sizes); ++i) {
				VideoSize* size = DSA_GetItemPtr(gVideo.sizes, i);
				UINT blocks = ((size->width + 15) / 16) * ((size->height + 15) / 16);
				for (UINT r = 0; r < _countof(gFps); ++r) {
					DWORD bit = 1u << r;
					if (size->supported & bit) continue;
					DXGI_RATIONAL rate = VideoRate(r);
					if (gFps[r] && gFps[r] >= (DWORD)MulDiv(gVideo.refresh.Numerator, 1, gVideo.refresh.Denominator)) continue;
					if (capacity && (UINT64)blocks * rate.Numerator > (UINT64)capacity * rate.Denominator) continue;
					IMFMediaType* type = Encoder_VideoType(&MFVideoFormat_H264, eAVEncH264VProfile_High,
						size->width >= 1280 || size->height > 576, size->width, size->height, rate);
					if (!type) continue;
					if (SUCCEEDED(IMFTransform_SetOutputType(codec, 0, type, 0))) {
						IMFMediaType* input = Encoder_InputType(&MFVideoFormat_NV12, size->width, size->height, rate);
						if (input) {
							if (SUCCEEDED(IMFTransform_SetInputType(codec, 0, input, MFT_SET_TYPE_TEST_ONLY))) {
								size->supported |= bit;
								if (hardware) size->hardware |= bit;
							}
							IMFMediaType_Release(input);
						}
					}
					IMFTransform_SetInputType(codec, 0, NULL, 0);
					IMFTransform_SetOutputType(codec, 0, NULL, 0);
					IMFMediaType_Release(type);
				}
			}
			IMFTransform_Release(codec);
			IMFActivate_ShutdownObject(list[e]);
		}
		IMFActivate_Release(list[e]);
	}
	CoTaskMemFree(list);
}

static BOOL LoadVideoModes(const RecorderMonitor* target)
{
	MONITORINFOEXW info = { .cbSize = sizeof(info) };
	if (!target || !GetMonitorInfoW(target->handle, (MONITORINFO*)&info)) return FALSE;
	DEVMODEW mode = { .dmSize = sizeof(mode) };
	EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
	DWORD width = info.rcMonitor.right - info.rcMonitor.left;
	DWORD height = info.rcMonitor.bottom - info.rcMonitor.top;
	DWORD fallback = mode.dmDisplayFrequency;
	if (fallback < 2) {
		DWM_TIMING_INFO timing = { .cbSize = sizeof(timing) };
		if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &timing)) && timing.rateCompose.uiDenominator)
			fallback = MulDiv(timing.rateCompose.uiNumerator, 1, timing.rateCompose.uiDenominator);
	}
	DXGI_RATIONAL refresh = target->refresh;
	if (!refresh.Numerator || !refresh.Denominator) refresh = (DXGI_RATIONAL){ fallback, 1 };
	if (!refresh.Numerator) return FALSE;
	if (gVideo.monitor == target->handle && gVideo.width == width && gVideo.height == height &&
		(UINT64)refresh.Numerator * gVideo.refresh.Denominator == (UINT64)gVideo.refresh.Numerator * refresh.Denominator) return TRUE;
	gVideo.monitor = NULL;
	gVideo.refresh = refresh;
	gVideo.width = width;
	gVideo.height = height;
	if (gVideo.sizes) DSA_Destroy(gVideo.sizes);
	gVideo.sizes = DSA_Create(sizeof(VideoSize), 8);
	if (!gVideo.sizes) return FALSE;
	AddVideoSize(width, height);
	for (DWORD i = 0; EnumDisplaySettingsW(info.szDevice, i, &mode); ++i)
		if (mode.dmPelsWidth <= width && mode.dmPelsHeight <= height)
			AddVideoSize(mode.dmPelsWidth, mode.dmPelsHeight);
	// Recording can also downscale without switching the desktop display mode.
	static const int shortEdges[] = { 240, 360, 480, 540, 720, 900, 1080, 1440, 2160, 4320 };
	for (UINT i = 0; i < _countof(shortEdges); ++i) {
		int edge = shortEdges[i];
		if ((DWORD)edge >= min(width, height)) break;
		if (width >= height) AddVideoSize(2 * MulDiv(width, edge, 2 * height), edge);
		else AddVideoSize(edge, 2 * MulDiv(height, edge, 2 * width));
	}
	ID3D11Device* device = CreateDevice();
	if (!device) return FALSE;
	if (gConfig.HardwareEncoder) ProbeVideoEncoder(device, TRUE);
	ProbeVideoEncoder(device, FALSE);
	ID3D11Device_Release(device);
	for (int i = 0; i < DSA_GetItemCount(gVideo.sizes); ++i) {
		if (((VideoSize*)DSA_GetItemPtr(gVideo.sizes, i))->supported) {
			gVideo.monitor = target->handle;
			return TRUE;
		}
	}
	return FALSE;
}

static VideoSize* SelectedVideoMode(UINT* rate)
{
	if (!gVideo.monitor) return NULL;
	VideoSize* best = NULL;
	DWORD bestArea = 0;
	for (int i = 0; i < DSA_GetItemCount(gVideo.sizes); ++i) {
		VideoSize* size = DSA_GetItemPtr(gVideo.sizes, i);
		if (!size->supported) continue;
		if (!best) best = size;
		if (size->width == gConfig.VideoMaxWidth && size->height == gConfig.VideoMaxHeight) { best = size; break; }
		DWORD area = size->width * size->height;
		if (area > bestArea && area <= (UINT64)gConfig.VideoMaxWidth * gConfig.VideoMaxHeight) {
			best = size;
			bestArea = area;
		}
	}
	if (!best) return NULL;
	DWORD wanted = gConfig.VideoMaxFramerate ? gConfig.VideoMaxFramerate : MAXDWORD;
	DWORD first;
	_BitScanForward(&first, best->supported);
	*rate = first;
	for (UINT i = 0; i < _countof(gFps); ++i) {
		DXGI_RATIONAL fps = VideoRate(i);
		if ((best->supported & (1u << i)) && (DWORD)MulDiv(fps.Numerator, 1, fps.Denominator) <= wanted) *rate = i;
	}
	return best;
}

static void VideoMenus(HMENU menu, const RecorderMonitor* monitor)
{
	UINT rate = 0;
	VideoSize* selected = LoadVideoModes(monitor) ? SelectedVideoMode(&rate) : NULL;
	HMENU fps = CreatePopupMenu(), sizes = CreatePopupMenu();
	WCHAR text[64];
	if (selected) {
		for (UINT i = 0; i < _countof(gFps); ++i) {
			if (!(selected->supported & (1u << i))) continue;
			if (gFps[i]) StrFormat(text, L"%u FPS", gFps[i]);
			else {
				UINT n = MulDiv(gVideo.refresh.Numerator, 100, gVideo.refresh.Denominator);
				if (n % 100) StrFormat(text, L"%u,%02u FPS (ecrã)", n / 100, n % 100);
				else StrFormat(text, L"%u FPS (ecrã)", n / 100);
			}
			AppendMenuW(fps, i == rate ? MF_CHECKED : MF_STRING, CMD_FPS_BASE + i, text);
		}
		for (int i = 0; i < DSA_GetItemCount(gVideo.sizes); ++i) {
			VideoSize* size = DSA_GetItemPtr(gVideo.sizes, i);
			if (!(size->supported & (1u << rate))) continue;
			StrFormat(text, L"%u × %u", size->width, size->height);
			AppendMenuW(sizes, size == selected ? MF_CHECKED : MF_STRING, CMD_SIZE_BASE + i, text);
		}
	}
	UINT flags = MF_POPUP | ((!selected || gRecording) ? MF_GRAYED : 0);
	AppendMenuW(menu, flags, (UINT_PTR)fps, L"FPS");
	AppendMenuW(menu, flags, (UINT_PTR)sizes, L"Resolução");
}
