/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : wavparser.c
  * @brief          : WAV file parser implementation
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

#include "wavparser.h"

static int const WAV_ID_RIFF = 0x52494646; /* "RIFF" */
static int const WAV_ID_WAVE = 0x57415645; /* "WAVE" */
static int const WAV_ID_FMT = 0x666d7420; /* "fmt " */
static int const WAV_ID_DATA = 0x64617461; /* "data" */

static int make_even_number_of_bytes_in_length(int x)
{
    if ((x & 0x01) != 0) {
        return x + 1;
    }
    return x;
}

static int read_32_bits_high_low(FIL * fp)
{
    unsigned char bytes[4] = { 0, 0, 0, 0 };
    UINT br;

    FRESULT res = f_read(fp, bytes, 4, &br);     /* Read a chunk of src file */
    if (res || br == 0) return -1;
    {
        int32_t const low = bytes[3];
        int32_t const medl = bytes[2];
        int32_t const medh = bytes[1];
        int32_t const high = (signed char) (bytes[0]);
        return (high << 24) | (medh << 16) | (medl << 8) | low;
    }
}

static int read_32_bits_low_high(FIL * fp)
{
    unsigned char bytes[4] = { 0, 0, 0, 0 };
    UINT br;

    FRESULT res = f_read(fp, bytes, 4, &br);     /* Read a chunk of src file */
    if (res || br == 0) return -1;
    {
        int32_t const low = bytes[0];
        int32_t const medl = bytes[1];
        int32_t const medh = bytes[2];
        int32_t const high = (signed char) (bytes[3]);
        return (high << 24) | (medh << 16) | (medl << 8) | low;
    }
}

static int read_16_bits_low_high(FIL * fp)
{
    unsigned char bytes[2] = { 0, 0 };
    UINT br;

    FRESULT res = f_read(fp, bytes, 2, &br);     /* Read a chunk of src file */
    if (res || br == 0) return -1;
    {
        int32_t const low = bytes[0];
        int32_t const high = (signed char) (bytes[1]);
        return (high << 8) | low;
    }
}

uint8_t parse_wave_header(FIL * sf, WAV_FMT* wav_fmt)
{
    int type;
    uint8_t loop_sanity = 0;
    int subSize = 0;
    FRESULT res;

    type = read_32_bits_high_low(sf);
    if(type != WAV_ID_RIFF) return 1;

    read_32_bits_low_high(sf); //wav_fmt->ChunkSize = read_32_bits_low_high(sf);

    type = read_32_bits_high_low(sf);
    if(type != WAV_ID_WAVE) return 1;

    for (loop_sanity = 0; loop_sanity < 20; ++loop_sanity)
    {
        type = read_32_bits_high_low(sf);

        if (type == WAV_ID_FMT)
        {
            subSize = read_32_bits_low_high(sf);
            subSize = make_even_number_of_bytes_in_length(subSize);
            if (subSize < 16) return 1; /*'fmt' chunk too short*/
            if(read_16_bits_low_high(sf) != 1) return 1;	//PCM = 1 (i.e. Linear quantization) Values other than 1 indicate some form of compression.
            subSize -= 2;
            wav_fmt->NumChannels = read_16_bits_low_high(sf);	//Mono = 1, Stereo = 2, etc.
            if(wav_fmt->NumChannels > 2) return 1;
            subSize -= 2;
            wav_fmt->SampleRate = read_32_bits_low_high(sf);	//8000, 44100, etc.
            subSize -= 4;
            read_32_bits_low_high(sf); //wav_fmt->ByteRate = read_32_bits_low_high(sf);	//== SampleRate * NumChannels * BitsPerSample/8
            subSize -= 4;
            read_16_bits_low_high(sf); //wav_fmt->BlockAlign = read_16_bits_low_high(sf);	//== NumChannels * BitsPerSample/8
            subSize -= 2;
            wav_fmt->BitsPerSample = read_16_bits_low_high(sf);	//8 bits = 8, 16 bits = 16, etc.
            subSize -= 2;

            /* WAVE_FORMAT_EXTENSIBLE support */
            if (subSize > 9)
            {
                read_16_bits_low_high(sf); /* cbSize */
                read_16_bits_low_high(sf); /* ValidBitsPerSample */
                read_32_bits_low_high(sf); /* ChannelMask */
                read_16_bits_low_high(sf); /* SubType coincident with format_tag for PCM int or float */
                subSize -= 10;
            }

            if (subSize > 0)
            {
                res = f_lseek(sf, f_tell(sf) + subSize);
                if(res) return 1;
            }
        }
        else if (type == WAV_ID_DATA)
        {
            subSize = read_32_bits_low_high(sf);
            wav_fmt->DataLen = subSize;
            /* We've found the audio data. Read no further! */
            break;

        }
        else
        {
            subSize = read_32_bits_low_high(sf);
            subSize = make_even_number_of_bytes_in_length(subSize);

            res = f_lseek(sf, f_tell(sf) + subSize);
            if(res) return 1;
        }
    }

    return 0;
}
