// Run from a Visual Studio x64 developer prompt in src:
// cl /std:c11 /experimental:c11atomics tests\microphone.c /link mfplat.lib mfuuid.lib ole32.lib && microphone.exe
#include "../wcap_microphone.h"
#include <assert.h>
#include <string.h>

int main(void)
{
	assert(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
	assert(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
	IMFSample* sample;
	IMFMediaBuffer* buffer;
	assert(SUCCEEDED(MFCreateSample(&sample)));
	const short playback[] = { 100, -100, 100, -100, 32760, -100, 10, -10, 10, -10 };
	const short expected[] = { 100, -100, 1100, 900, 32767, 1900, 10, -10, -990, -1010 };
	assert(SUCCEEDED(MFCreateMemoryBuffer(sizeof(playback), &buffer)));
	BYTE* data;
	assert(SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, NULL)));
	memcpy(data, playback, sizeof(playback));
	IMFMediaBuffer_Unlock(buffer);
	IMFMediaBuffer_SetCurrentLength(buffer, sizeof(playback));
	IMFSample_AddBuffer(sample, buffer);
	IMFSample_SetSampleTime(sample, 0);

	// The voice starts after playback and briefly goes silent between two voiced samples.
	Microphone_Store((short[]){1000, 2000}, 2, MIC_RATE + 1);
	Microphone_Store((short[]){0, -1000}, 2, MIC_RATE + 3);
	Microphone_Mix(sample, MF_UNITS_PER_SECOND, MIC_RATE, 2);
	assert(SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, NULL)));
	assert(memcmp(data, expected, sizeof(expected)) == 0);
	IMFMediaBuffer_Unlock(buffer);
	Microphone_Stop();
	assert(gMic.First == gMic.End);
	IMFMediaBuffer_Release(buffer);
	IMFSample_Release(sample);
	MFShutdown();
	CoUninitialize();
	return 0;
}

