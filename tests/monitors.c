// After build.cmd, run in a Visual Studio x64 developer prompt:
// cl /O1 /DNDEBUG /std:c11 /experimental:c11atomics tests\monitors.c /link /subsystem:console && monitors.exe
#include "../universal_recorder.c"

int main(void)
{
	RecorderMonitor a = { .handle = (HMONITOR)1, .name = L"27B2", .path = L"target-a" };
	RecorderMonitor b = { .handle = (HMONITOR)2, .name = L"DELL U2723QE", .path = L"target-b" };
	gMonitors = DSA_Create(sizeof(RecorderMonitor), 2);
	DSA_AppendItem(gMonitors, &a);
	HMENU root = CreatePopupMenu();
	MonitorMenu(root);
	if (GetMenuItemCount(root) != 0) return 1;

	DSA_AppendItem(gMonitors, &b);
	StrCpyW(gConfig.MonitorPath, b.path);
	MonitorMenu(root);
	HMENU menu = GetSubMenu(root, 0);
	WCHAR text[128];
	if (!menu || GetMenuItemCount(menu) != 3) return 2;
	GetMenuStringW(menu, 0, text, _countof(text), MF_BYPOSITION);
	if (lstrcmpW(text, L"Automatic (monitor under the pointer)")) return 9;
	GetMenuStringW(menu, 2, text, _countof(text), MF_BYPOSITION);
	if (lstrcmpW(text, b.name) || !(GetMenuState(menu, 2, MF_BYPOSITION) & MF_CHECKED)) return 3;
	if (SelectedMonitor(a.handle)->handle != b.handle) return 4;
	DSA_DeleteItem(gMonitors, 0);
	if (SelectedMonitor(a.handle)->handle != b.handle) return 5;
	DSA_SetItem(gMonitors, 0, &a);
	if (SelectedMonitor(a.handle)->handle != a.handle) return 6;
	DSA_AppendItem(gMonitors, &b);
	if (SelectedMonitor(a.handle)->handle != b.handle) return 7;
	gConfig.MonitorPath[0] = 0;
	if (SelectedMonitor(a.handle)->handle != a.handle) return 8;
	DestroyMenu(root);
	DSA_Destroy(gMonitors);
	return 0;
}
