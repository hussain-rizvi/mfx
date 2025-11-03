/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_audio_player.h
  * @brief          : USB Audio Player for WAV playback
  ******************************************************************************
  */

#ifndef __USB_AUDIO_PLAYER_H
#define __USB_AUDIO_PLAYER_H

#include "main.h"
#include "fatfs.h"
#include "usbh_audio.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio player states */
typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_READY,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_ERROR
} PlayerState;

/* File types */
typedef enum {
    FILE_NONE = 0,
    FILE_WAV
} FileType;

/* Function prototypes */
void USBAudioPlayer_Init(void);
void USBAudioPlayer_Process(void);
void USBAudioPlayer_Start(void);
void USBAudioPlayer_Stop(void);
PlayerState USBAudioPlayer_GetState(void);

/* Buffer empty callback - called by USB Host when buffer needs refill */
void USBAudioPlayer_BufferEmptyCallback(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_AUDIO_PLAYER_H */

