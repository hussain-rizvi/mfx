/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : mp3dec.h
  * @brief          : MP3 decoder header file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef _MP3DEC_H
#define _MP3DEC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* determining MAINBUF_SIZE:
 *   max mainDataBegin = (2^9 - 1) bytes (since 9-bit offset) = 511
 *   max nSlots (concatenated with mainDataBegin bytes from before) = 1440 - 9 - 4 + 1 = 1428
 *   511 + 1428 = 1939, round up to 1940 (4-byte align)
 */
#define MAINBUF_SIZE	1940

#define MAX_NGRAN		2		/* max granules */
#define MAX_NCHAN		2		/* max channels */
#define MAX_NSAMP		576		/* max samples per channel, per granule */

#define WAVBUFFER8_SIZE	16512
#define WAVBUFFER16_SIZE	8256
#define WAVBUFFER24_SIZE	5504
#define WAVBUFFER32_SIZE	4128

/* map to 0,1,2 to make table indexing easier */
typedef enum {
	MPEG1 =  0,
	MPEG2 =  1,
	MPEG25 = 2
} MPEGVersion;

typedef void *HMP3Decoder;

enum {
	ERR_MP3_NONE =                  0,
	ERR_MP3_INDATA_UNDERFLOW =     -1,
	ERR_MP3_MAINDATA_UNDERFLOW =   -2,
	ERR_MP3_FREE_BITRATE_SYNC =    -3,
	ERR_MP3_OUT_OF_MEMORY =	       -4,
	ERR_MP3_NULL_POINTER =         -5,
	ERR_MP3_INVALID_FRAMEHEADER =  -6,
	ERR_MP3_INVALID_SIDEINFO =     -7,
	ERR_MP3_INVALID_SCALEFACT =    -8,
	ERR_MP3_INVALID_HUFFCODES =    -9,
	ERR_MP3_INVALID_DEQUANTIZE =   -10,
	ERR_MP3_INVALID_IMDCT =        -11,
	ERR_MP3_INVALID_SUBBAND =      -12,

	ERR_UNKNOWN =                  -9999
};

typedef union _AudioBuffer {
	struct {
		unsigned char WAVBuffer8_1[WAVBUFFER8_SIZE];
		unsigned char WAVBuffer8_2[WAVBUFFER8_SIZE];
	} WAVBuffer8;
	struct {
		short WAVBuffer16_1[WAVBUFFER16_SIZE];
		short WAVBuffer16_2[WAVBUFFER16_SIZE];
	} WAVBuffer16;
	struct {
		unsigned char WAVBuffer24_1[WAVBUFFER24_SIZE][3];
		unsigned char WAVBuffer24_2[WAVBUFFER24_SIZE][3];
	} WAVBuffer24;
	struct {
		int WAVBuffer32_1[WAVBUFFER32_SIZE];
		int WAVBuffer32_2[WAVBUFFER32_SIZE];
	} WAVBuffer32;
	struct {	//33032 Bytes
		unsigned char mp3DecInfoB[2032];
		unsigned char fhB[56];
		unsigned char siB[328];
		unsigned char sfiB[284];
		unsigned char hiB[4624];
		unsigned char diB[840];
		unsigned char miB[6944];
		unsigned char sbiB[8708];
		short PCMbuffer1[2304];
		short PCMbuffer2[2304];
	} MP3Buffer;
} AudioBuffer;

extern AudioBuffer DecoderAudioBuffer;

typedef struct _MP3FrameInfo {
	int bitrate;
	int nChans;
	int samprate;
	int bitsPerSample;
	int outputSamps;
	int layer;
	int version;
} MP3FrameInfo;

/* public API */
HMP3Decoder MP3InitDecoder(void);
void MP3FreeDecoder(HMP3Decoder hMP3Decoder);
int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf, int *bytesLeft, short *outbuf, int useSize);

void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo);
int MP3GetNextFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo, unsigned char *buf);
int MP3FindSyncWord(unsigned char *buf, int nBytes);

#ifdef __cplusplus
}
#endif

#endif	/* _MP3DEC_H */
