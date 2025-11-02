/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : wavparser.h
  * @brief          : WAV file parser header file
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

#ifndef _WAVPARSER_H
#define _WAVPARSER_H

#include "stm32h7xx.h"
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    //uint32_t ChunkSize;
    uint32_t NumChannels;
    uint32_t SampleRate;
    //uint32_t ByteRate;
    //uint32_t BlockAlign;
    uint32_t BitsPerSample;
    uint32_t DataLen;
    //uint32_t DataStart;
} WAV_FMT;

/* Function prototypes */
uint8_t parse_wave_header(FIL *sf, WAV_FMT *wav_fmt);

#ifdef __cplusplus
}
#endif

#endif /* _WAVPARSER_H */
