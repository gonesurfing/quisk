/*
 * This module provides sound access for QUISK using the Windows WASAPI library.

 * This software is Copyright (C) 2020 by James C. Ahlstrom, and is
 * licensed for use under the GNU General Public License (GPL).
 * See http://www.opensource.org.
 * Note that there is NO WARRANTY AT ALL.  USE AT YOUR OWN RISK!!

*/
#define UNICODE
#include <Python.h>
#include <complex.h>
#include <math.h>
#include "quisk.h"
#include <stdint.h>
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <Ksmedia.h>
#include <processthreadsapi.h>

int quisk_midi_cwkey;

#define EXIT_ON_ERROR(hres)  \
	if (FAILED(hres)) { goto Exit; }

// REFERENCE_TIME time units per second and per millisecond
#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000
#define REFTIMES_PER_MICROSEC   10

#define PKEY_DEVICE_FRIENDLYNAME (&PKEY_Device_FriendlyName)
#define CLSID_MMDEVICEENUMERATOR (&CLSID_MMDeviceEnumerator)
#define IID_IAUDIOCLIENT (&IID_IAudioClient)
#define IID_IAUDIORENDERCLIENT (&IID_IAudioRenderClient)
#define IID_IAUDIOCAPTURECLIENT (&IID_IAudioCaptureClient)
#define IID_IMMDEVICEENUMERATOR (&IID_IMMDeviceEnumerator)

const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {
	0x00000003,0x0000,0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
} ;

const GUID KSDATAFORMAT_SUBTYPE_PCM = {
	0x00000001,0x0000,0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
} ;

static IMMDeviceEnumerator *pEnumerator = NULL;

struct dev_data_t {
	IMMDevice *pDevice;
	IAudioClient *pAudioClient;
	IAudioCaptureClient *pCaptureClient;
	IAudioRenderClient *pRenderClient;
	UINT32 bufferSizeFrames;
	HANDLE hEvent;
	int playbuf_read_reset;		// playbuf is used for the play device threadproc
	int playbuf_write_reset;
	int playbuf_nFrames;
	int playbuf_iRead;
	int playbuf_iWrite;
	unsigned char * playbuf_buf;	// samples are IQIQIQ...
} ;

static LPWSTR to_pwsz(const char *str) {	// Convert UTF-8 string to Wide Character string.
	static wchar_t wchar_buffer[256];
	if (MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, str, -1, wchar_buffer, 256) == 0)
		return NULL;
	return wchar_buffer;
}

static void close_device(struct sound_dev *, const char *);
static void open_wasapi_playback(struct sound_dev *, int);

static DWORD WINAPI playdevice_threadproc(LPVOID lpParameter)	// Called by a special thread.
{  // Read samples from the playdevice threadproc buffer and write them to the sound card.
	struct sound_dev * dev = (struct sound_dev *)lpParameter;
	struct dev_data_t * DD = dev->device_data;
	DWORD retval;
	BYTE * pData;
	BYTE * ptSamples;
	int i, two, ch_I, ch_Q, frames_in_buffer, bytes_per_sample, bytes_per_frame, iRead;
	DWORD taskIndex = 0;
	HANDLE hTask;

	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK)
		QuiskPrintf("%s:  CoInitializeEx failed\n", dev->stream_description);
	hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
	if (hTask == NULL)
		QuiskPrintf("%s:  Could not set thread to Pro Audio\n", dev->stream_description);
	if (FAILED(IAudioClient_GetService(DD->pAudioClient, IID_IAUDIORENDERCLIENT, (void**)&DD->pRenderClient))) {
		close_device(dev, "Could not create playback client");
		return 1;
	}
	// Load the buffer with silence before starting the stream.
	if (SUCCEEDED(IAudioRenderClient_GetBuffer(DD->pRenderClient, DD->bufferSizeFrames, &pData)))
		IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, DD->bufferSizeFrames, AUDCLNT_BUFFERFLAGS_SILENT);
	dev->handle = DD->pRenderClient;
	if (FAILED(IAudioClient_Start(DD->pAudioClient))) {
		close_device(dev, "Could not start");
		return 1;
	}
	if (quisk_sound_state.verbose_sound) {
		QuiskPrintf("  Playback buffer size %d frames\n", DD->bufferSizeFrames);
		QuiskPrintf("  Callback ring buffer size %d frames\n", DD->playbuf_nFrames);
		QuiskPrintf("  Started.\n");
	}
	while (1) {
		// Wait for next buffer event to be signaled.
		retval = WaitForSingleObject(DD->hEvent, 1000);
		if (retval != WAIT_OBJECT_0)
			break;
		if (DD == NULL || dev->handle == NULL)
			break;
		if (IAudioRenderClient_GetBuffer(DD->pRenderClient, DD->bufferSizeFrames, &pData) != S_OK) {
			QuiskPrintf("%s:  playdevice: GetBuffer failed\n", dev->stream_description);
			continue;
		}
		if (quisk_play_state < RECEIVE) {
			DD->playbuf_iRead = 0;
			DD->playbuf_read_reset = 0;
			IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, DD->bufferSizeFrames, AUDCLNT_BUFFERFLAGS_SILENT);
			continue;
		}
		iRead = DD->playbuf_iRead;
		frames_in_buffer = DD->playbuf_iWrite - iRead;       // must be atomic
		if (frames_in_buffer < 0)
			frames_in_buffer += DD->playbuf_nFrames;
		if (DD->playbuf_read_reset) {
			if (frames_in_buffer >= DD->playbuf_nFrames / 2) {
				DD->playbuf_read_reset = 0;
			}
			else {
				IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, DD->bufferSizeFrames, AUDCLNT_BUFFERFLAGS_SILENT);
				continue;
			}
		}
		else if (frames_in_buffer <= DD->bufferSizeFrames) {
			DD->playbuf_read_reset = 1;
			dev->dev_underrun++;
			IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, DD->bufferSizeFrames, AUDCLNT_BUFFERFLAGS_SILENT);
			continue;
		}
		two = dev->num_channels >= 2;
		ch_I = dev->channel_I;
		ch_Q = dev->channel_Q;
		bytes_per_sample = dev->sample_bytes;
		bytes_per_frame = bytes_per_sample * dev->num_channels;
		if ((rxMode == CWU || rxMode == CWL) && quiskSpotLevel < 0 && dev->dev_index == t_MicPlayback) {	// SoftRock Tx CW from wasapi
			for (i = 0; i < DD->bufferSizeFrames; i++) {
				ptSamples = quisk_make_txIQ(dev, 0);
				memcpy(pData + ch_I * bytes_per_sample, ptSamples, bytes_per_sample);
				if (two) {
					ptSamples += bytes_per_sample;
					memcpy(pData + ch_Q * bytes_per_sample, ptSamples, bytes_per_sample);
				}
				pData += bytes_per_frame;
			}
		}
		else if (quisk_play_state > RECEIVE && quisk_active_sidetone == 1 && dev->dev_index == t_Playback) {	// Sidetone from wasapi
			for (i = 0; i < DD->bufferSizeFrames; i++) {
				ptSamples = quisk_make_sidetone(dev, 0);
				memcpy(pData + ch_I * bytes_per_sample, ptSamples, bytes_per_sample);
				if (two)
					memcpy(pData + ch_Q * bytes_per_sample, ptSamples, bytes_per_sample);
				pData += bytes_per_frame;
			}
		}
		else {		// copy sound samples from the buffer to the sound device.
			ptSamples = DD->playbuf_buf + iRead * bytes_per_sample * 2;
			if (dev->num_channels == 2 && ch_I == 0 && ch_Q == 1 && iRead + DD->bufferSizeFrames < DD->playbuf_nFrames) {	// check for fast copy
				memcpy(pData, ptSamples, DD->bufferSizeFrames * bytes_per_frame);
			}
			else {
				for (i = 0; i < DD->bufferSizeFrames; i++) {
					memcpy(pData + ch_I * bytes_per_sample, ptSamples, bytes_per_sample);
					ptSamples += bytes_per_sample;
					if (two)
						memcpy(pData + ch_Q * bytes_per_sample, ptSamples, bytes_per_sample);
					ptSamples += bytes_per_sample;
					pData += bytes_per_frame;
					if (++iRead >= DD->playbuf_nFrames) {	// end of buffer; go to start
						iRead = 0;
						ptSamples = DD->playbuf_buf;
					}
				}
			}
		}
		IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, DD->bufferSizeFrames, 0);
		iRead = DD->playbuf_iRead + DD->bufferSizeFrames;	// update read index
		if (iRead >= DD->playbuf_nFrames)
			iRead -= DD->playbuf_nFrames;
		DD->playbuf_iRead = iRead;	// must be atomic
	}
	AvRevertMmThreadCharacteristics(hTask);
	if (quisk_sound_state.verbose_sound)
		QuiskPrintf("%s: Exit playback thread\n", dev->stream_description);
	CoUninitialize();
	return 0;
}

void quisk_write_wasapi(struct sound_dev * dev, int nSamples, complex double * cSamples, double volume)
{	// Called from Quisk by the sound thread. Write samples to the playdevice threadproc buffer.
	// The ring buffer always stores samples as: IQIQIQIQ...
	struct dev_data_t * DD = dev->device_data;
	int i, frames_in_buffer, iWrite;
	int8_t * ptInt8;
	int16_t * ptInt16;
	int32_t * ptInt32;
	float   * ptFloat;
	int32_t samp32;
	unsigned char * buffer_end;
	double tm;
	float buffer_fill;
	//QuiskPrintTime("write play buffer", 0);
	//QuiskMeasureRate("write_wasapi", nSamples, 1, 1);
	if (quisk_play_state < RECEIVE)
		return;
	if (DD == NULL || dev->handle == NULL)
		return;
	iWrite = DD->playbuf_iWrite;
	frames_in_buffer = iWrite - DD->playbuf_iRead;		// must be atomic
	if (frames_in_buffer < 0)
		frames_in_buffer += DD->playbuf_nFrames;
	dev->dev_latency = frames_in_buffer + nSamples;		// frames in buffer available to play
	buffer_fill = (float)dev->dev_latency / DD->playbuf_nFrames;
	if (quisk_sound_state.verbose_sound) {
		if (frames_in_buffer <= DD->bufferSizeFrames)
			QuiskPrintf("%s:  playdevice: buffer underflow\n", dev->stream_description);
		if (quisk_sound_state.verbose_sound > 1) {
			tm = QuiskTimeSec();
			if (tm - dev->TimerTime0 > 10.000) {
				QuiskPrintf("%s:  Buffer fill %.2f%%\n", dev->stream_description, buffer_fill * 100);
				dev->TimerTime0 = tm;
			}
		}
	}
	if (DD->playbuf_write_reset) {
		if (frames_in_buffer <= DD->playbuf_nFrames / 2) {
			DD->playbuf_write_reset = 0;
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("%s:  play_buffer overflow recovery frames_in_buffer %d, nSamples %d\n", dev->stream_description,
					frames_in_buffer, nSamples);
		}
		else {
			//QuiskPrintf("frames_in_buffer %d, nSamples %d\n", frames_in_buffer, nSamples);
			return;
		}
	}
	if (frames_in_buffer + nSamples >= DD->playbuf_nFrames) {
		if (quisk_sound_state.verbose_sound)
			QuiskPrintf("%s:  play_buffer overflow frames_in_buffer %d, nSamples %d\n", dev->stream_description,
					frames_in_buffer, nSamples);
		dev->dev_error++;
		nSamples = DD->playbuf_nFrames * 9 / 10 - frames_in_buffer;     // almost fill buffer
		if (nSamples < 0)      // buffer is nearly full
			DD->playbuf_write_reset = 1;
	}
	else if (buffer_fill > 0.7 && nSamples > 0) {
		nSamples--;	// buffer too full, remove a sample
	}
	else if (buffer_fill < 0.3 && nSamples >= 2) {
		cSamples[nSamples] = cSamples[nSamples - 1];
		cSamples[nSamples - 1] = (cSamples[nSamples - 2] + cSamples[nSamples]) / 2.0;
		nSamples++;	// buffer too empty, add a sample
	}
	if (nSamples <= 0)
		return;
	buffer_end = DD->playbuf_buf + DD->playbuf_nFrames * dev->sample_bytes * 2;
	switch (dev->sound_format) {
	case Int16:
		ptInt16 = (int16_t *)(DD->playbuf_buf + iWrite * dev->sample_bytes * 2);
		for (i = 0; i < nSamples; i++) {		// for each frame
			*ptInt16++ = volume * creal(cSamples[i]) / 65536;
			*ptInt16++ = volume * cimag(cSamples[i]) / 65536;
			if ((unsigned char *)ptInt16 >= buffer_end) {
				iWrite = 0;
				ptInt16 = (int16_t *)DD->playbuf_buf;
			}
		}
		break;
	case Int24:     // only works for little-endian
		ptInt8 = (int8_t *)(DD->playbuf_buf + iWrite * dev->sample_bytes * 2);
		for (i = 0; i < nSamples; i++) {
			samp32 = volume * creal(cSamples[i]);
			memcpy(ptInt8, (int8_t *)&samp32 + 1, 3);
			ptInt8 += 3;
			samp32 = volume * cimag(cSamples[i]);
			memcpy(ptInt8, (int8_t *)&samp32 + 1, 3);
			ptInt8 += 3;
			if ((unsigned char *)ptInt8 >= buffer_end) {
				iWrite = 0;
				ptInt8 = (int8_t *)DD->playbuf_buf;
			}
		}
		break;
	case Int32:
		ptInt32 = (int32_t *)(DD->playbuf_buf + iWrite * dev->sample_bytes * 2);
		for (i = 0; i < nSamples; i++) {
			*ptInt32++ = volume * creal(cSamples[i]);
			*ptInt32++ = volume * cimag(cSamples[i]);
			if ((unsigned char *)ptInt32 >= buffer_end) {
				iWrite = 0;
				ptInt32 = (int32_t *)DD->playbuf_buf;
			}
		}
		break;
	case Float32:
		ptFloat = (float *)(DD->playbuf_buf + iWrite * dev->sample_bytes * 2);
		for (i = 0; i < nSamples; i++) {
			*ptFloat++ = volume * creal(cSamples[i]) / CLIP32;
			*ptFloat++ = volume * cimag(cSamples[i]) / CLIP32;
			if ((unsigned char *)ptFloat >= buffer_end) {
				iWrite = 0;
				ptFloat = (float *)DD->playbuf_buf;
			}
		}
		break;
	}
	iWrite = DD->playbuf_iWrite + nSamples;
	if (iWrite >= DD->playbuf_nFrames)
		iWrite -= DD->playbuf_nFrames;
	DD->playbuf_iWrite = iWrite;	// must be atomic
}

int quisk_read_wasapi(struct sound_dev * dev, complex double * cSamples)
{	// Called from Quisk by the sound thread. Read samples from the sound card capture buffer.
	// cSamples can be NULL to discard samples.
	struct dev_data_t * DD = dev->device_data;
	int i;
	BYTE * pData;
	UINT32 numFramesAvailable;
	DWORD flags;
	float timer;
	double samp_r, samp_i;
	int16_t * ptInt16;
	int32_t * ptInt32;
	float   * ptFloat32;
	int nSamples;
	int second_try;
	HRESULT hr;

	if (DD == NULL || dev->handle == NULL)
		return 0;

	nSamples = 0;
	second_try = 0;
	while (1) {	// Get the available data
		hr = IAudioCaptureClient_GetBuffer(DD->pCaptureClient, &pData, &numFramesAvailable, &flags, NULL, NULL);
		if (hr == AUDCLNT_S_BUFFER_EMPTY) {
			if (dev->read_frames <= 0)	// non-blocking read, we are finished
				break;
			if (second_try)		// sleep only once
				break;
			second_try = 1;
			timer = (float)(dev->read_frames - nSamples) / dev->sample_rate;
			if (timer > 0.004 && timer < 0.100) {
				QuiskSleepMicrosec((int)(timer * 1E6));
				continue;	// try again
			}
			break;
		}
		if (hr != S_OK) {	// Error
			QuiskPrintf("%s:  Error processing buffer\n", dev->stream_description);
			break;
		}
		// There is data to read
		if (cSamples) {
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
				for (i = 0; i < numFramesAvailable; i++)
					cSamples[nSamples++] = 0;
			}
			else switch (dev->sound_format) {
			case Int16:
				ptInt16 = (int16_t *)pData;
				for (i = 0; i < numFramesAvailable; i++) {
					samp_r = *ptInt16++;
					samp_i = *ptInt16++;
					cSamples[nSamples++] = (samp_r + I * samp_i) * CLIP16;
				}
				break;
			case Int24:
				break;
			case Int32:
				ptInt32 = (int32_t *)pData;
				for (i = 0; i < numFramesAvailable; i++) {
					samp_r = *ptInt32++;
					samp_i = *ptInt32++;
					cSamples[nSamples++] = (samp_r + I * samp_i);
				}
				break;
			case Float32:
				ptFloat32 = (float *)pData;
				for (i = 0; i < numFramesAvailable; i++) {
					samp_r = *ptFloat32++;
					samp_i = *ptFloat32++;
					cSamples[nSamples++] = (samp_r + I * samp_i) * CLIP32;
				}
				break;
			}
		}
		if (FAILED(IAudioCaptureClient_ReleaseBuffer(DD->pCaptureClient, numFramesAvailable))) {
			QuiskPrintf("%s:  Failure to release buffer\n", dev->stream_description);
			break;
		}
	}
	dev->dev_latency = nSamples;
	return nSamples;
}

void quisk_play_wasapi(struct sound_dev * dev, int nSamples, complex double * cSamples, double volume)
{	// Called from Quisk by the sound thread. Write samples to the sound card buffer.
	UINT32 numFramesPadding;
	BYTE *pData;
	int16_t * pts;
	int32_t * ptl;
	float * ptf;
	struct dev_data_t * DD = dev->device_data;
	float buffer_fill;
	int n, frames;
	double tm;

	if (DD == NULL || dev->handle == NULL)
		return;
	if (FAILED(IAudioClient_GetCurrentPadding(DD->pAudioClient, &numFramesPadding))) {
		QuiskPrintf("%s:  quisk_play_wasapi failed to get padding\n", dev->stream_description);
		return;
	}
	buffer_fill = (float)(numFramesPadding + nSamples) / DD->bufferSizeFrames;
	if (quisk_sound_state.verbose_sound > 1) {
		tm = QuiskTimeSec();
		if (tm - dev->TimerTime0 > 10.000) {
			QuiskPrintf("%s:  Buffer Fill %.2f%%\n", dev->stream_description, buffer_fill * 100);
			dev->TimerTime0 = tm;
		}
	}
	dev->dev_latency = numFramesPadding + nSamples;		// frames in buffer available to play
	switch(dev->started) {
	case 0:	 // Starting state; wait for samples to become regular
		if (quisk_play_state < RECEIVE) {
			frames = DD->bufferSizeFrames / 2 - (int)numFramesPadding;
			if (frames > 0 && SUCCEEDED(IAudioRenderClient_GetBuffer(DD->pRenderClient, frames, &pData))) {
				IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, frames, AUDCLNT_BUFFERFLAGS_SILENT);
			}
			return;
		}
		dev->started = 1;
		if (quisk_sound_state.verbose_sound)
			QuiskPrintf("%s:  Starting\n", dev->stream_description);
		// FALL THRU
	case 1:	// Normal run state
		// Check for underrun
		if (numFramesPadding <= dev->sample_rate / 200) {	// mimumum play time in buffer
			quisk_sound_state.underrun_error++;
			dev->dev_underrun++;
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf ("%s:  Underrun error\n", dev->stream_description);
			for (nSamples = 0; nSamples < DD->bufferSizeFrames / 2; nSamples++)
				cSamples[nSamples] = 0;   // fill with silence
			buffer_fill = 0.5;
		}
		// Check if play buffer is too full.
		if (numFramesPadding + nSamples >= DD->bufferSizeFrames) {
			quisk_sound_state.write_error++;
			dev->dev_error++;
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf ("%s:  Buffer too full\n", dev->stream_description);
			nSamples = 0;
			dev->started = 2;
		}
		else if (buffer_fill > 0.7 && nSamples >= 1) {
			nSamples--;
#if DEBUG_IO
			QuiskPrintf("%s:  play_alsa Remove a sample\n", dev->stream_description);
#endif
		}
		else if(buffer_fill < 0.3 && nSamples >= 2) {
			cSamples[nSamples] = cSamples[nSamples - 1];
			cSamples[nSamples - 1] = (cSamples[nSamples - 2] + cSamples[nSamples]) / 2.0;
			nSamples++;
#if DEBUG_IO
			QuiskPrintf ("%s:  play_alsa Add a sample\n", dev->stream_description);
#endif
		}
		break;
	case 2:	 // Buffer is too full; wait for it to drain
		if (buffer_fill <= 0.5) {
			dev->started = 1;
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("%s:  Resume adding samples\n", dev->stream_description);
		}
		else {
			nSamples = 0;
		}
		break;
	}
	if (nSamples <= 0)
		return;
	if (FAILED(IAudioRenderClient_GetBuffer(DD->pRenderClient, nSamples, &pData))) {
		QuiskPrintf("%s:  quisk_play_wasapi failed get the buffer\n", dev->stream_description);
		return;
	}
	switch (dev->sound_format) {
	case Int16:
		pts = (int16_t *)pData;
		for (n = 0; n < nSamples; n++) {
			pts[dev->channel_I] = (int16_t)(volume * creal(cSamples[n]) / 65536);
			pts[dev->channel_Q] = (int16_t)(volume * cimag(cSamples[n]) / 65536);
			pts += dev->num_channels;
		}
		break;
	case Int32:
		ptl = (int32_t *)pData;
		for (n = 0; n < nSamples; n++) {
			ptl[dev->channel_I] = (int32_t)(volume * creal(cSamples[n]));
			ptl[dev->channel_Q] = (int32_t)(volume * cimag(cSamples[n]));
			ptl += dev->num_channels;
		}
		break;
	case Float32:
		ptf = (float *)pData;
		for (n = 0; n < nSamples; n++) {
			ptf[dev->channel_I] = (volume * creal(cSamples[n]) / CLIP32);
			ptf[dev->channel_Q] = (volume * cimag(cSamples[n]) / CLIP32);
			ptf += dev->num_channels;
		}
		break;
	case Int24:
		break;
	}
	IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, nSamples, 0);
}

static void MakeWFext(int extensible, sound_format_t sound_format, WORD nChannels, struct sound_dev * dev, WAVEFORMATEXTENSIBLE * pwfex)
{	// fill in a WAVEFORMATEXTENSIBLE structure
	dev->sound_format = sound_format;
	switch (sound_format) {
	case Int16:
		dev->use_float = 0;
		dev->sample_bytes = 2;
		break;
	case Int24:
		dev->use_float = 0;
		dev->sample_bytes = 3;
		break;
	case Int32:
		dev->use_float = 0;
		dev->sample_bytes = 4;
		break;
	case Float32:
		dev->use_float = 1;
		dev->sample_bytes = 4;
		break;
	}
	if (extensible) {
		pwfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		pwfex->Format.cbSize = 22;
		pwfex->Samples.wValidBitsPerSample = dev->sample_bytes * 8;
		switch (nChannels) {
		case 1:
			pwfex->dwChannelMask = 0x01;
			break;
		case 2:
			pwfex->dwChannelMask = 0x03;
			break;
		case 3:
			pwfex->dwChannelMask = 0x07;
			break;
		case 4:
			pwfex->dwChannelMask = 0x0F;
			break;
		case 5:
			pwfex->dwChannelMask = 0x1F;
			break;
		case 6:
			pwfex->dwChannelMask = 0x3F;
			break;
		case 7:
		default:
			pwfex->dwChannelMask = 0x7F;
			break;
		}
		if (sound_format == Float32)
			pwfex->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
		else
			pwfex->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	}
	else {
		pwfex->Format.cbSize = 0;
		if (sound_format == Float32)
			pwfex->Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		else
			pwfex->Format.wFormatTag = WAVE_FORMAT_PCM;
	}
	pwfex->Format.nChannels = nChannels;
	pwfex->Format.nSamplesPerSec = dev->sample_rate;
	pwfex->Format.nAvgBytesPerSec = nChannels * dev->sample_rate * dev->sample_bytes;
	pwfex->Format.nBlockAlign = nChannels * dev->sample_bytes;
	pwfex->Format.wBitsPerSample = dev->sample_bytes * 8;
}

static int make_format(struct sound_dev * dev, WAVEFORMATEXTENSIBLE * pWaveFormat)
{
	struct dev_data_t * DD = dev->device_data;
	WAVEFORMATEX * pEx = (WAVEFORMATEX *)pWaveFormat;
	WORD nChannels;
	sound_format_t sound_format;

	nChannels = dev->num_channels;
	for (sound_format = 0; sound_format <= Float32; sound_format++) {
		MakeWFext(1, sound_format, nChannels, dev, pWaveFormat);
		if (IAudioClient_IsFormatSupported(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, pEx, NULL) == S_OK)
			return 1;
	}
	if (dev->dev_index == t_Playback || dev->dev_index == t_MicCapture) {	// mono device is OK for radio sound or mic
		for (sound_format = 0; sound_format <= Float32; sound_format++) {
			MakeWFext(1, sound_format, 1, dev, pWaveFormat);
			if (IAudioClient_IsFormatSupported(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, pEx, NULL) == S_OK) {
				dev->num_channels = 1;
				dev->channel_I = 0;
				dev->channel_Q = 0;
				return 1;
			}
		}
	}
	// Try to open with more channels
	for (nChannels = dev->num_channels + 1; nChannels <= 7; nChannels++) {
		for (sound_format = 0; sound_format <= Float32; sound_format++) {
			MakeWFext(1, sound_format, nChannels, dev, pWaveFormat);
			if (IAudioClient_IsFormatSupported(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, pEx, NULL) == S_OK) {
				dev->num_channels = nChannels;
				return 1;
			}
		}
	}
	return 0;
}

static void close_device(struct sound_dev * dev, const char * msg)
{
	struct dev_data_t * device_data = dev->device_data;

	if (msg) {
		snprintf(dev->dev_errmsg, QUISK_SC_SIZE, "%s: %.80s", msg, dev->name);
		if (quisk_sound_state.verbose_sound)
			QuiskPrintf("%s\n", dev->dev_errmsg);
	}
	if (device_data->pAudioClient)
		IAudioClient_Stop(device_data->pAudioClient);
	if (device_data->pCaptureClient)
		IAudioCaptureClient_Release(device_data->pCaptureClient);
	if (device_data->pRenderClient)
		IAudioRenderClient_Release(device_data->pRenderClient);
	if (device_data->pAudioClient)
		IAudioClient_Release(device_data->pAudioClient);
	if (device_data->pDevice)
		IMMDevice_Release(device_data->pDevice);
	if (device_data->playbuf_buf)
		free(device_data->playbuf_buf);
	if (device_data->hEvent)
		CloseHandle(device_data->hEvent);
	free(device_data);
	dev->device_data = NULL;
}

static void open_wasapi_capture(struct sound_dev * dev)
{
	REFERENCE_TIME hnsRequestedDuration;
	REFERENCE_TIME def_period, min_period;
	WAVEFORMATEXTENSIBLE wave_format;
	LPWSTR pwszID;
	struct dev_data_t * DD = dev->device_data;
	HRESULT hr;

	dev->dev_errmsg[0] = 0;
	if (quisk_sound_state.verbose_sound)
		QuiskPrintf("Opening Wasapi capture device %s\n  Name %s\n  Device name %s\n", dev->stream_description, dev->name, dev->device_name);
	if (pEnumerator == NULL)
		return;

	pwszID = to_pwsz(dev->device_name);
	if (pwszID == NULL) {
		close_device(dev, "Failure to convert device name");
		return;
	}
	if (FAILED(IMMDeviceEnumerator_GetDevice(pEnumerator, pwszID, &DD->pDevice))) {
		close_device(dev, "Sound device not found");
		return;
	}

	if (FAILED(IMMDevice_Activate(DD->pDevice, IID_IAUDIOCLIENT, CLSCTX_ALL, NULL, (void**)&DD->pAudioClient))) {
		close_device(dev, "No audio client");
		return;
	}

	if (SUCCEEDED(IAudioClient_GetDevicePeriod(DD->pAudioClient, &def_period, &min_period))) {
		if (quisk_sound_state.verbose_sound)
			QuiskPrintf("  Device default period %d, min period %d microsec\n",
				(int)def_period / REFTIMES_PER_MICROSEC, (int)min_period / REFTIMES_PER_MICROSEC);
	}

	if (make_format(dev, &wave_format) == 0) {
		close_device(dev, "Device can not support the sample rate or number of channels.");
		return;
	}

	if (quisk_sound_state.verbose_sound) {
		QuiskPrintf("  Sample rate %d\n  Channel_I %d\n  Channel_Q %d\n",
			dev->sample_rate, dev->channel_I, dev->channel_Q);
		QuiskPrintf("  Sound device format %s\n", sound_format_names[dev->sound_format]);
		QuiskPrintf("  Number of channels %d, bytes per sample %d\n", dev->num_channels, dev->sample_bytes);
	}
	hnsRequestedDuration = REFTIMES_PER_MILLISEC * quisk_sound_state.latency_millisecs;
	hr = IAudioClient_Initialize(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, hnsRequestedDuration, 0, (WAVEFORMATEX *)&wave_format, NULL);
	if (FAILED(hr)) {
		for ( ;hnsRequestedDuration > REFTIMES_PER_MILLISEC * 20; hnsRequestedDuration -= (REFTIMES_PER_MILLISEC * 20)) {
			hr = IAudioClient_Initialize(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, hnsRequestedDuration, 0, (WAVEFORMATEX *)&wave_format, NULL);
			if (hr == S_OK)
				break;
		}
	}
	if (FAILED(hr)) {
		close_device(dev, "Could not initialize");
		return;
	}

	// Get the size of the allocated buffer.
	if (FAILED(IAudioClient_GetBufferSize(DD->pAudioClient, &DD->bufferSizeFrames))) {
		close_device(dev, "Could not get buffer size");
		return;
	}
	dev->play_buf_size = DD->bufferSizeFrames * dev->sample_bytes * dev->num_channels;

	if (FAILED(IAudioClient_GetService(DD->pAudioClient, IID_IAUDIOCAPTURECLIENT, (void**)&DD->pCaptureClient))) {
		close_device(dev, "Could not create capture client");
		return;
	}

	dev->handle = DD->pCaptureClient;

	if (FAILED(IAudioClient_Start(DD->pAudioClient))) {
		close_device(dev, "Could not start");
		return;
	}
	if (quisk_sound_state.verbose_sound) {
		QuiskPrintf("  Capture buffer size %d frames\n  Started.\n", DD->bufferSizeFrames);
	}
}

static void open_wasapi_playback(struct sound_dev * dev, int use_callback)
{
	REFERENCE_TIME hnsRequestedDuration = 0;
	REFERENCE_TIME def_period, min_period;
	WAVEFORMATEXTENSIBLE wave_format;
	BYTE *pData;
	LPWSTR pwszID;
	struct dev_data_t * DD = dev->device_data;
	HRESULT hr = S_OK;
	HANDLE hThread = NULL;
	DWORD ThreadID;

	dev->dev_errmsg[0] = 0;
	if (quisk_sound_state.verbose_sound) {
		QuiskPrintf("Opening Wasapi playback device %s\n  Name %s\n  Device name %s\n", dev->stream_description, dev->name, dev->device_name);
	}
	if (pEnumerator == NULL)
		return;

	pwszID = to_pwsz(dev->device_name);
	if (pwszID == NULL) {
		close_device(dev, "Failure to convert device name");
		return;
	}
	if (FAILED(IMMDeviceEnumerator_GetDevice(pEnumerator, pwszID, &DD->pDevice))) {
		close_device(dev, "Sound device not found");
		return;
	}

	if (FAILED(IMMDevice_Activate(DD->pDevice, IID_IAUDIOCLIENT, CLSCTX_ALL, NULL, (void**)&DD->pAudioClient))) {
		close_device(dev, "No audio client");
		return;
	}

	if (SUCCEEDED(IAudioClient_GetDevicePeriod(DD->pAudioClient, &def_period, &min_period))) {
		if (quisk_sound_state.verbose_sound)
			QuiskPrintf("  Device default period %d, min period %d microsec\n",
				(int)def_period / REFTIMES_PER_MICROSEC, (int)min_period / REFTIMES_PER_MICROSEC);
	}

	if (make_format(dev, &wave_format) == 0) {
		close_device(dev, "Device can not support the sample rate or number of channels.");
		return;
	}
	if (quisk_sound_state.verbose_sound) {
		QuiskPrintf("  Sample rate %d\n  Channel_I %d\n  Channel_Q %d\n",
			dev->sample_rate, dev->channel_I, dev->channel_Q);
		QuiskPrintf("  Sound device format %s\n", sound_format_names[dev->sound_format]);
		QuiskPrintf("  Number of channels %d, bytes per sample %d\n", dev->num_channels, dev->sample_bytes);
	}
	if (use_callback) {
		hnsRequestedDuration = REFTIMES_PER_MICROSEC * quisk_sound_state.data_poll_usec;
		if (hnsRequestedDuration < min_period)
			hnsRequestedDuration = min_period;
		hr = IAudioClient_Initialize(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			hnsRequestedDuration, hnsRequestedDuration, (WAVEFORMATEX *)&wave_format, NULL);
		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("  Fix for buffer size not aligned\n");
			// Get the size of the aligned buffer.
			if (FAILED(IAudioClient_GetBufferSize(DD->pAudioClient, &DD->bufferSizeFrames))) {
				close_device(dev, "Could not get buffer size");
				return;
			}
			hnsRequestedDuration = (REFERENCE_TIME)((10000.0 * 1000 / wave_format.Format.nSamplesPerSec * DD->bufferSizeFrames) + 0.5);
			IAudioClient_Release(DD->pAudioClient);
			if (FAILED(IMMDevice_Activate(DD->pDevice, IID_IAUDIOCLIENT, CLSCTX_ALL, NULL, (void**)&DD->pAudioClient))) {
				close_device(dev, "No audio client");
				return;
			}
			hr = IAudioClient_Initialize(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				hnsRequestedDuration, hnsRequestedDuration, (WAVEFORMATEX *)&wave_format, NULL);
		}
		if (hr != S_OK) {
			close_device(dev, "Could not initialize");
			return;
		}
		// Create an event handle and register it for buffer-event notifications.
		DD->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (DD->hEvent == NULL) {
			close_device(dev, "CreateEvent failed");
			return;
		}
		if (FAILED(IAudioClient_SetEventHandle(DD->pAudioClient, DD->hEvent))) {
			close_device(dev, "SetEventHandle failed");
			return;
		}
		DD->playbuf_nFrames = dev->latency_frames * 2;
		DD->playbuf_buf = calloc(DD->playbuf_nFrames * dev->sample_bytes * 2, 1);
		DD->playbuf_iWrite = DD->playbuf_nFrames / 2;   // buffer starts half full
		DD->playbuf_iRead = 0;
		DD->playbuf_read_reset = 0;
		DD->playbuf_write_reset = 0;
		hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) playdevice_threadproc, dev, CREATE_SUSPENDED, &ThreadID);
		if (hThread == NULL ) {
			close_device(dev, "Create thread failed");
			return;
		}
		// Get the size of the allocated buffer.
		if (FAILED(IAudioClient_GetBufferSize(DD->pAudioClient, &DD->bufferSizeFrames))) {
			close_device(dev, "Could not get buffer size");
			return;
		}
		dev->play_buf_size = DD->bufferSizeFrames * dev->sample_bytes * dev->num_channels;
		ResumeThread(hThread);
	}
	else {
		hnsRequestedDuration = quisk_sound_state.latency_millisecs * REFTIMES_PER_MILLISEC * 2;		// reduce this if too large
		for ( ;hnsRequestedDuration > REFTIMES_PER_MILLISEC * 20; hnsRequestedDuration -= (REFTIMES_PER_MILLISEC * 20)) {
			hr = IAudioClient_Initialize(DD->pAudioClient, AUDCLNT_SHAREMODE_EXCLUSIVE, 0,
				hnsRequestedDuration, 0, (WAVEFORMATEX *)&wave_format, NULL);
			if (hr == S_OK)
				break;
		}
		if (FAILED(hr)) {
			close_device(dev, "Could not initialize");
			return;
		}

		// Get the size of the allocated buffer.
		if (FAILED(IAudioClient_GetBufferSize(DD->pAudioClient, &DD->bufferSizeFrames))) {
			close_device(dev, "Could not get buffer size");
			return;
		}
		dev->play_buf_size = DD->bufferSizeFrames * dev->sample_bytes * dev->num_channels;

		if (FAILED(IAudioClient_GetService(DD->pAudioClient, IID_IAUDIORENDERCLIENT, (void**)&DD->pRenderClient))) {
			close_device(dev, "Could not create playback client");
			return;
		}
		// Load the buffer with silence before starting the stream.
		if (SUCCEEDED(IAudioRenderClient_GetBuffer(DD->pRenderClient, DD->bufferSizeFrames / 2, &pData)))
			IAudioRenderClient_ReleaseBuffer(DD->pRenderClient, DD->bufferSizeFrames / 2, AUDCLNT_BUFFERFLAGS_SILENT);
		dev->handle = DD->pRenderClient;
		if (FAILED(IAudioClient_Start(DD->pAudioClient))) {
			close_device(dev, "Could not start");
			return;
		}
		if (quisk_sound_state.verbose_sound) {
			QuiskPrintf("  Playback buffer size %d frames\n", DD->bufferSizeFrames);
			if (use_callback)
				QuiskPrintf("  Callback ring buffer size %d frames\n", DD->playbuf_nFrames);
			QuiskPrintf("  Started.\n");
		}
	}
}

void quisk_start_sound_wasapi (struct sound_dev ** pCapture, struct sound_dev ** pPlayback)
{  // Open the sound devices and start them. Called from the sound thread.
	struct sound_dev * pDev;

	if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK)
		QuiskPrintf("CoInitializeEx failed\n");
	if (FAILED(CoCreateInstance(CLSID_MMDEVICEENUMERATOR, NULL, CLSCTX_ALL, IID_IMMDEVICEENUMERATOR, (void**)&pEnumerator))) {
		QuiskPrintf("CoCreateInstance failed in start_sound_wasapi\n");
		pEnumerator = NULL;
		return;
	}
	while (*pCapture) {
		pDev = *pCapture++;
		if (pDev->driver == DEV_DRIVER_WASAPI) {
			pDev->device_data = calloc(sizeof(struct dev_data_t), 1);
			open_wasapi_capture(pDev);
		}
	}
	while (*pPlayback) {
		pDev = *pPlayback++;
		if (pDev->driver == DEV_DRIVER_WASAPI) {
			pDev->device_data = calloc(sizeof(struct dev_data_t), 1);
			open_wasapi_playback(pDev, 0);
		}
		else if (pDev->driver == DEV_DRIVER_WASAPI2) {
			pDev->device_data = calloc(sizeof(struct dev_data_t), 1);
			open_wasapi_playback(pDev, 1);
		}
	}
}

void quisk_close_sound_wasapi(struct sound_dev ** pCapture, struct sound_dev ** pPlayback)
{
	struct sound_dev * pDev;

	while (*pCapture) {
		pDev = *pCapture++;
		if (pDev->driver == DEV_DRIVER_WASAPI && pDev->device_data) {
			pDev->handle = NULL;
			Sleep(200);
			close_device(pDev, NULL);
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("Close %s\n", pDev->stream_description);
		}
	}
	while (*pPlayback) {
		pDev = *pPlayback++;
		if ((pDev->driver == DEV_DRIVER_WASAPI || pDev->driver == DEV_DRIVER_WASAPI2) && pDev->device_data) {
			pDev->handle = NULL;
			Sleep(200);
			close_device(pDev, NULL);
			if (quisk_sound_state.verbose_sound)
				QuiskPrintf("Close %s\n", pDev->stream_description);
		}
	}
	if (pEnumerator)
		IMMDeviceEnumerator_Release(pEnumerator);
	pEnumerator = NULL;
	CoUninitialize();
}


PyObject * quisk_wasapi_sound_devices(PyObject * self, PyObject * args)		// Called from the GUI thread
{	// Return a list of sound device data [pycapt, pyplay]
	PyObject * pylist, * pycapt, * pyplay, * pytup;
	HRESULT hr = S_OK;
	IMMDeviceEnumerator *pEnum = NULL;
	IMMDeviceCollection *pCollection = NULL;
	IMMDevice *pEndpoint = NULL;
	IPropertyStore *pProps = NULL;
	PROPVARIANT varName;
	LPWSTR pwszID = NULL;
	UINT i, count;

	if (!PyArg_ParseTuple (args, ""))
		return NULL;
	pylist = PyList_New(0);		// list [pycapt, pyplay]
	pycapt = PyList_New(0);		// list of capture devices (name, id, is_raw)
	pyplay = PyList_New(0);		// list of play devices
	PyList_Append(pylist, pycapt);
	PyList_Append(pylist, pyplay);

	hr = CoCreateInstance(CLSID_MMDEVICEENUMERATOR, NULL, CLSCTX_ALL, IID_IMMDEVICEENUMERATOR, (void**)&pEnum);
	if (FAILED(hr)) {
		QuiskPrintf("CoCreateInstance failed in wasapi_sound_devices\n");
		return pylist;
	}
	hr = IMMDeviceEnumerator_EnumAudioEndpoints(pEnum, eRender, DEVICE_STATE_ACTIVE, &pCollection);
	EXIT_ON_ERROR(hr)
	hr = IMMDeviceCollection_GetCount(pCollection, &count);
	EXIT_ON_ERROR(hr)
	for (i = 0; i < count; i++) {   // Get pointer to eRender endpoint number i.
		hr = IMMDeviceCollection_Item(pCollection, i, &pEndpoint);
		EXIT_ON_ERROR(hr)
		hr = IMMDevice_GetId(pEndpoint, &pwszID);
		EXIT_ON_ERROR(hr)
		hr = IMMDevice_OpenPropertyStore(pEndpoint, STGM_READ, &pProps);
		EXIT_ON_ERROR(hr)
		PropVariantInit(&varName);
		hr = IPropertyStore_GetValue(pProps, &PKEY_Device_FriendlyName, &varName);
		EXIT_ON_ERROR(hr)
		// data items are (name, id, is_raw)
		pytup = PyTuple_New(3);
		PyList_Append(pyplay, pytup);
		PyTuple_SET_ITEM(pytup, 0, PyUnicode_FromWideChar(varName.pwszVal, -1));
		PyTuple_SET_ITEM(pytup, 1, PyUnicode_FromWideChar(pwszID, -1));
		PyTuple_SET_ITEM(pytup, 2, PyInt_FromLong(1));
		CoTaskMemFree(pwszID);
		pwszID = NULL;
		PropVariantClear(&varName);
		IPropertyStore_Release(pProps);
		pProps = NULL;
		IMMDevice_Release(pEndpoint);
		pEndpoint = NULL;
	}
	IMMDeviceCollection_Release(pCollection);
	pCollection = NULL;
	hr = IMMDeviceEnumerator_EnumAudioEndpoints(pEnum, eCapture, DEVICE_STATE_ACTIVE, &pCollection);
	EXIT_ON_ERROR(hr)
	hr = IMMDeviceCollection_GetCount(pCollection, &count);
	EXIT_ON_ERROR(hr)
	for (i = 0; i < count; i++) {   // Get pointer to eCapture endpoint number i.
		hr = IMMDeviceCollection_Item(pCollection, i, &pEndpoint);
		EXIT_ON_ERROR(hr)
		hr = IMMDevice_GetId(pEndpoint, &pwszID);
		EXIT_ON_ERROR(hr)
		hr = IMMDevice_OpenPropertyStore(pEndpoint, STGM_READ, &pProps);
		EXIT_ON_ERROR(hr)
		PropVariantInit(&varName);
		hr = IPropertyStore_GetValue(pProps, &PKEY_Device_FriendlyName, &varName);
		EXIT_ON_ERROR(hr)
		// data items are (name, id, is_raw)
		pytup = PyTuple_New(3);
		PyList_Append(pycapt, pytup);
		PyTuple_SET_ITEM(pytup, 0, PyUnicode_FromWideChar(varName.pwszVal, -1));
		PyTuple_SET_ITEM(pytup, 1, PyUnicode_FromWideChar(pwszID, -1));
		PyTuple_SET_ITEM(pytup, 2, PyInt_FromLong(1));
		CoTaskMemFree(pwszID);
		pwszID = NULL;
		PropVariantClear(&varName);
		IPropertyStore_Release(pProps);
		pProps = NULL;
		IMMDevice_Release(pEndpoint);
		pEndpoint = NULL;
	}
	IMMDeviceCollection_Release(pCollection);
	IMMDeviceEnumerator_Release(pEnum);
	return pylist;

Exit:
	if (pwszID)
		CoTaskMemFree(pwszID);
	if (pCollection)
		IMMDeviceCollection_Release(pCollection);
	if (pEndpoint)
		IMMDevice_Release(pEndpoint);
	if (pProps)
		IPropertyStore_Release(pProps);
	if (pEnum)
		IMMDeviceEnumerator_Release(pEnum);
	return pylist;
}

/******  MIDI  ******/

static void midi_in_devices(PyObject * pylist)
{	// Return a list of MIDI In devices.
	UINT nMidiDeviceNum;
	MIDIINCAPS caps;
	UINT i;

	nMidiDeviceNum = midiInGetNumDevs();
	for (i = 0; i < nMidiDeviceNum; ++i) {
		if (midiInGetDevCaps(i, &caps, sizeof(MIDIINCAPS)) == MMSYSERR_NOERROR)
			PyList_Append(pylist, PyUnicode_FromWideChar(caps.szPname, -1));
	}
}

void CALLBACK MidiInProc(HMIDIIN hMidiIn, UINT wMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
{
	int msg, note, velocity;

	if (wMsg == MIM_DATA) {
		msg = (dwParam1 >> 4) & 0xF;
		note = (dwParam1 >> 8) & 0x7F;
		velocity = (dwParam1 >> 16) & 0x7F;
		//QuiskPrintf ("instance %ld msg %d note %d veloc %d\n", dwInstance, msg, note, velocity);
		if (note == dwInstance) {       // dwInstance is the CW note
			if (msg == 9) {		// Note ON
				if (velocity)
					quisk_midi_cwkey = 1;
				else
					quisk_midi_cwkey = 0;
			}
			else if (msg == 8) {     // Note OFF
				quisk_midi_cwkey = 0;
			}
		}
	}
}

PyObject * quisk_control_midi(PyObject * self, PyObject * args, PyObject * keywds)
{  /* Call with keyword arguments ONLY */
	static char * kwlist[] = {"client", "device", "close_port", "get_event", "midi_cwkey_note",
					"get_in_names", "get_in_devices", NULL} ;
	int client, close_port, get_event, get_in_names, get_in_devices;
	static int midi_cwkey_note = -1;
	char * device;
	PyObject * pylist;
	static HMIDIIN hMidiDevice = NULL;;

	client = close_port = get_event = get_in_names = get_in_devices = -1;
	device = NULL;
	if (!PyArg_ParseTupleAndKeywords (args, keywds, "|isiiiii", kwlist,
			&client, &device, &close_port, &get_event, &midi_cwkey_note, &get_in_names, &get_in_devices))
		return NULL;
	if (close_port == 1) {	// shutdown
		if (hMidiDevice) {
			midiInStop(hMidiDevice);
			midiInClose(hMidiDevice);
		}
		hMidiDevice = NULL;
		quisk_midi_cwkey = 0;
	}
	if (get_in_devices == 1) {	// return a list of MIDI devices; just a list of names
		pylist = PyList_New(0);
		midi_in_devices(pylist);
		return pylist;
	}
	if (get_in_names == 1) {	// return a list of MIDI devices; just a list of names
		pylist = PyList_New(0);
		midi_in_devices(pylist);
		return pylist;
	}
	if (client >= 0) {		// open client port
		quisk_midi_cwkey = 0;
		if (hMidiDevice == NULL) {
			if (midiInOpen(&hMidiDevice, client, (DWORD_PTR)(void*)MidiInProc, midi_cwkey_note, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
				QuiskPrintf("Could not open MIDI device\n");
				hMidiDevice = NULL;
			}
			else {
				midiInStart(hMidiDevice);
				if (quisk_sound_state.verbose_sound)
					QuiskPrintf("Open MIDI device %d\n", client);
			}
		}
	}
	if (get_event == 1)		// poll to get event
		;
	Py_INCREF (Py_None);
	return Py_None;
}
