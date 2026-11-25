#pragma once

#include "wcap.h"
#include <dmo.h>
#include <mfapi.h>
#include <propsys.h>
EXTERN_C const GUID FORMAT_WaveFormatEx;
#include <wmcodecdsp.h>

#pragma comment(lib, "dmoguids")

enum { MIC_RATE = 16000, MIC_HISTORY = 32768 };
static struct {
	IMediaObject* Source;
	IMediaBuffer* Buffer;
	LONGLONG First, End;
	short Samples[MIC_HISTORY];
} gMic;

static void Microphone_Stop(void)
{
	if (gMic.Source) {
		IMediaObject_FreeStreamingResources(gMic.Source);
		IMediaObject_Release(gMic.Source);
		gMic.Source = NULL;
	}
	if (gMic.Buffer) {
		IMediaBuffer_Release(gMic.Buffer);
		gMic.Buffer = NULL;
	}
	gMic.First = gMic.End = 0;
	SecureZeroMemory(gMic.Samples, sizeof(gMic.Samples));
}

static HRESULT Microphone_Start(void)
{
	IPropertyStore* props = NULL;
	IMFMediaBuffer* memory = NULL;
	HRESULT hr = CoCreateInstance(&CLSID_CWMAudioAEC, NULL, CLSCTX_INPROC_SERVER, &IID_IMediaObject, (void**)&gMic.Source);
	if (SUCCEEDED(hr)) hr = IMediaObject_QueryInterface(gMic.Source, &IID_IPropertyStore, (void**)&props);
	// This Windows voice mode applies noise suppression and automatic gain control.
	PROPVARIANT mode = { .vt = VT_I4, .lVal = SINGLE_CHANNEL_NSAGC };
	PROPVARIANT enabled = { .vt = VT_BOOL, .boolVal = VARIANT_TRUE };
	if (SUCCEEDED(hr)) hr = IPropertyStore_SetValue(props, &MFPKEY_WMAAECMA_SYSTEM_MODE, &mode);
	if (SUCCEEDED(hr)) hr = IPropertyStore_SetValue(props, &MFPKEY_WMAAECMA_DMO_SOURCE_MODE, &enabled);
	if (props) IPropertyStore_Release(props);

	WAVEFORMATEX format = { WAVE_FORMAT_PCM, 1, MIC_RATE, MIC_RATE * 2, 2, 16, 0 };
	DMO_MEDIA_TYPE type = { .majortype = MFMediaType_Audio, .subtype = MFAudioFormat_PCM,
		.bFixedSizeSamples = TRUE, .formattype = FORMAT_WaveFormatEx,
		.cbFormat = sizeof(format), .pbFormat = (BYTE*)&format };
	if (SUCCEEDED(hr)) hr = IMediaObject_SetOutputType(gMic.Source, 0, &type, 0);
	if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(MIC_RATE * sizeof(short), &memory);
	if (SUCCEEDED(hr)) hr = MFCreateLegacyMediaBufferOnMFMediaBuffer(NULL, memory, 0, &gMic.Buffer);
	if (memory) IMFMediaBuffer_Release(memory);
	if (SUCCEEDED(hr)) hr = IMediaObject_AllocateStreamingResources(gMic.Source);
	if (FAILED(hr)) Microphone_Stop();
	return hr;
}

// Keep processed voice on the device clock so silence or late packets cannot move it ahead of the video.
static void Microphone_Store(const short* samples, DWORD count, LONGLONG start)
{
	if (!gMic.End || start < gMic.First || start - gMic.End >= MIC_HISTORY) {
		gMic.First = gMic.End = start;
	}
	if (start >= gMic.End - 2 && start <= gMic.End + 2) start = gMic.End;
	while (gMic.End < start) gMic.Samples[gMic.End++ % MIC_HISTORY] = 0;
	for (DWORD i = (DWORD)min((LONGLONG)count, gMic.End - start); i < count; ++i)
		gMic.Samples[gMic.End++ % MIC_HISTORY] = samples[i];
	gMic.First = max(gMic.First, gMic.End - MIC_HISTORY);
}

static HRESULT Microphone_Read(LONGLONG now)
{
	DMO_OUTPUT_DATA_BUFFER output;
	do {
		output = (DMO_OUTPUT_DATA_BUFFER){ .pBuffer = gMic.Buffer };
		DWORD status, bytes;
		BYTE* data;
		HRESULT hr = IMediaBuffer_SetLength(gMic.Buffer, 0);
		if (SUCCEEDED(hr)) hr = IMediaObject_ProcessOutput(gMic.Source, 0, 1, &output, &status);
		if (SUCCEEDED(hr)) hr = IMediaBuffer_GetBufferAndLength(gMic.Buffer, &data, &bytes);
		if (FAILED(hr) || !bytes) return hr;
		DWORD count = bytes / sizeof(short);
		LONGLONG start = MFllMulDiv(output.rtTimestamp, MIC_RATE, MF_UNITS_PER_SECOND, 0);
		LONGLONG current = MFllMulDiv(now, MIC_RATE, MF_UNITS_PER_SECOND, 0);
		if (start <= 0 || start > current + MIC_RATE / 10 || start < current - 2 * MIC_RATE)
			start = gMic.End > current - 2 * MIC_RATE ? gMic.End : current - count;
		Microphone_Store((const short*)data, count, start);
	} while (output.dwStatus & DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE);
	return S_OK;
}

static void Microphone_Mix(IMFSample* sample, LONGLONG epoch, DWORD rate, DWORD channels)
{
	LONGLONG time;
	if (gMic.First >= gMic.End || FAILED(IMFSample_GetSampleTime(sample, &time))) return;
	IMFMediaBuffer* buffer;
	if (FAILED(IMFSample_GetBufferByIndex(sample, 0, &buffer))) return;
	BYTE* data;
	DWORD bytes;
	if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &bytes))) {
		short* pcm = (short*)data;
		DWORD frames = bytes / (sizeof(short) * channels);
		LONGLONG phase = MFllMulDiv(epoch + time, (LONGLONG)MIC_RATE * rate, MF_UNITS_PER_SECOND, 0);
		for (DWORD i = 0; i < frames; ++i, phase += MIC_RATE, pcm += channels) {
			LONGLONG pos = phase / rate;
			if (pos < gMic.First || pos >= gMic.End) continue;
			int a = gMic.Samples[pos % MIC_HISTORY];
			int b = gMic.Samples[min(pos + 1, gMic.End - 1) % MIC_HISTORY];
			int voice = a + (int)((b - a) * (phase % rate) / rate);
			for (DWORD ch = 0; ch < channels; ++ch) {
				int sum = pcm[ch] + voice;
				pcm[ch] = (short)max(-32768, min(32767, sum));
			}
		}
		IMFMediaBuffer_Unlock(buffer);
	}
	IMFMediaBuffer_Release(buffer);
}
