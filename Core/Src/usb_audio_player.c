/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_audio_player.c
  * @brief          : USB Audio Player for WAV playback from SD card
  ******************************************************************************
  */

#include "usb_audio_player.h"
#include "usb_host.h"
#include "usbh_audio.h"
#include "wavparser.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* External references */
extern USBH_HandleTypeDef hUsbHostHS;
extern ApplicationTypeDef Appli_state;
extern FATFS SDFatFS;
extern char SDPath[4];

/* Current file being played */
static FIL currentFile;
static char fileList[30][256];
static uint8_t fileCount = 0;
static uint8_t currentFileIndex = 0;
static FileType currentFileType = FILE_NONE;

/* Audio buffers */
static WAV_FMT WaveFormat;

/* Player state */
static PlayerState playerState = PLAYER_IDLE;
static bool fileOpen = false;
static bool wav_playback_started = false;
static bool frequencySetComplete = false;

/* WAV playback circular buffer tracking */
#define WAV_BUFFER_SIZE (512 * 33)  /* 16896 bytes - matches reference implementation */
static uint8_t wav_circular_buffer[WAV_BUFFER_SIZE];
static uint32_t wav_buffer_in_ptr = 0;   /* Write position in circular buffer */
static uint32_t wav_buffer_out_ptr = 0;  /* Reserved for future use */

/* Access to audio buffer from main.c */
extern uint8_t audio_buffer[];

/* Forward declarations */
static void PlayNextWAVFile(void);

/**
  * @brief Find audio files (WAV) on SD card
  */
static void FindAudioFiles(void)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;
    
    fileCount = 0;
    
    res = f_opendir(&dir, "/");
    if (res == FR_OK) {
        while (1) {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0) break;
            
            if (fno.fname[0] == '.') continue;
            
    /* Check if file has audio extension */
    char *ext = strrchr(fno.fname, '.');
            if (ext != NULL) {
                if (strcasecmp(ext, ".wav") == 0 || 
                    strcasecmp(ext, ".WAV") == 0) {
                    if (fileCount < 30) {
                        strcpy(fileList[fileCount], fno.fname);
                        fileCount++;
                    }
                }
            }
        }
        f_closedir(&dir);
    }
    
    /* Sort files alphabetically */
    for (uint8_t i = 0; i < fileCount; i++) {
        for (uint8_t j = i + 1; j < fileCount; j++) {
            if (strcmp(fileList[i], fileList[j]) > 0) {
                char temp[256];
                strcpy(temp, fileList[i]);
                strcpy(fileList[i], fileList[j]);
                strcpy(fileList[j], temp);
            }
        }
    }
    
}

/**
  * @brief Initialize the audio player
  */
void USBAudioPlayer_Init(void)
{
    /* Scan SD card for audio files */
    FindAudioFiles();
    
    printf("USBAudioPlayer: Found %d audio files\r\n", fileCount);
    
    if (fileCount == 0) {
        playerState = PLAYER_ERROR;
        printf("USBAudioPlayer: ERROR - No audio files found\r\n");
        return;
    }
    
    /* Initialize player state */
    currentFileIndex = 0;
    playerState = PLAYER_READY;
    fileOpen = false;
    
    printf("USBAudioPlayer: Initialized and ready\r\n");
}

/**
  * @brief Process the audio player state machine
  */
void USBAudioPlayer_Process(void)
{
    if (playerState != PLAYER_PLAYING) {
        return;
    }
    
    if (Appli_state != APPLICATION_READY) {
        return;
    }
    
    if (!fileOpen) {
        return;
    }
        
    /* Process based on file type */
    if (currentFileType == FILE_WAV) {
        /* Set USB audio frequency before starting playback */
        /* Note: SetFrequency requires play_state == AUDIO_PLAYBACK_IDLE (value 5) */
        if (!frequencySetComplete && !wav_playback_started && Appli_state == APPLICATION_READY && hUsbHostHS.gState == 11) {
            AUDIO_HandleTypeDef *AUDIO_Handle = NULL;
            if (hUsbHostHS.pActiveClass != NULL && hUsbHostHS.pActiveClass->pData != NULL) {
                AUDIO_Handle = (AUDIO_HandleTypeDef *) hUsbHostHS.pActiveClass->pData;
                
                /* Only call SetFrequency if play_state is IDLE */
                if (AUDIO_Handle->play_state == 5) {
                    USBH_StatusTypeDef freqStatus = USBH_AUDIO_SetFrequency(&hUsbHostHS, WaveFormat.SampleRate, 
                                            WaveFormat.NumChannels, WaveFormat.BitsPerSample);
                    
                    if (freqStatus == USBH_OK && hUsbHostHS.pActiveClass != NULL && hUsbHostHS.pActiveClass->pData != NULL) {
                        AUDIO_Handle = (AUDIO_HandleTypeDef *) hUsbHostHS.pActiveClass->pData;
                        frequencySetComplete = true;
                    }
                }
            }
        }
        
        /* Start playback after frequency is set */
        /* Uses full circular buffer and file size - matches reference implementation */
        if (frequencySetComplete && !wav_playback_started && hUsbHostHS.pActiveClass != NULL && hUsbHostHS.pActiveClass->pData != NULL) {
            AUDIO_HandleTypeDef *AUDIO_Handle = (AUDIO_HandleTypeDef *) hUsbHostHS.pActiveClass->pData;
            if (AUDIO_Handle->play_state == 5) {  /* IDLE state */
                USBH_StatusTypeDef playStatus = USBH_AUDIO_Play(&hUsbHostHS, wav_circular_buffer, WaveFormat.DataLen);
                if (playStatus == USBH_OK) {
                    wav_playback_started = true;
                }
            }
        }
        
        /* Circular buffer refill using polling approach - matches reference implementation */
        if (wav_playback_started) {
            /* Get current USB consumption position */
            int32_t out_ptr = USBH_AUDIO_GetOutOffset(&hUsbHostHS);
            
            if (out_ptr < 0) {
                /* End of file or error condition - play next WAV file */
                PlayNextWAVFile();
            } else if (out_ptr >= WAV_BUFFER_SIZE) {
                /* Buffer wrap-around - reset with ChangeOutBuffer */
                USBH_AUDIO_ChangeOutBuffer(&hUsbHostHS, wav_circular_buffer);
            } else {
                /* Calculate available space: difference between read and write positions */
                int32_t diff = (int32_t)out_ptr - (int32_t)wav_buffer_in_ptr;
                
                /* Handle wrap-around: if diff is negative, add buffer size */
                if (diff < 0) {
                    diff = WAV_BUFFER_SIZE + diff;
                }
                
                /* Refill when at least half the buffer is available */
                /* This maintains continuous playback by keeping buffer well-filled */
                if (diff >= (WAV_BUFFER_SIZE / 2)) {
                    /* Advance write pointer by one block (512 bytes) */
                    wav_buffer_in_ptr += 512;
                    
                    /* Wrap write pointer if buffer boundary reached */
                    if (wav_buffer_in_ptr >= WAV_BUFFER_SIZE) {
                        wav_buffer_in_ptr = 0;
                    }
                    
                    /* Read one block (512 bytes) from file into circular buffer */
                    UINT bytesRead;
                    FRESULT readRes = f_read(&currentFile, &wav_circular_buffer[wav_buffer_in_ptr], 
                                            512, &bytesRead);
                    if (readRes == FR_OK) {
                        if (bytesRead == 0) {
                            /* End of file reached - play next WAV file */
                            PlayNextWAVFile();
                        }
                    } else {
                        /* File read error - try next file */
                        PlayNextWAVFile();
                    }
                }
            }
        }
    }
}

/**
  * @brief Play next WAV file in sequence
  */
static void PlayNextWAVFile(void)
{
    /* Stop USB audio playback before closing file to prevent hardfault */
    if (wav_playback_started && Appli_state == APPLICATION_READY) {
        USBH_AUDIO_Stop(&hUsbHostHS);
        /* Wait for USB audio to properly stop (play_state should return to IDLE = 5) */
        if (hUsbHostHS.pActiveClass != NULL && hUsbHostHS.pActiveClass->pData != NULL) {
            AUDIO_HandleTypeDef *AUDIO_Handle = (AUDIO_HandleTypeDef *) hUsbHostHS.pActiveClass->pData;
            uint32_t timeout = HAL_GetTick() + 100; /* 100ms timeout */
            while (AUDIO_Handle->play_state != 5 && HAL_GetTick() < timeout) {
                /* Wait for play_state to return to IDLE (5) */
            }
        }
    }
    
    /* Close current file */
    if (fileOpen) {
        f_close(&currentFile);
        fileOpen = false;
    }
    
    /* Reset WAV playback state */
    wav_playback_started = false;
    /* Note: frequencySetComplete will be reset after new file header is parsed */
    wav_buffer_in_ptr = 0;
    wav_buffer_out_ptr = 0;
    
    /* Find next WAV file */
    uint8_t startIndex = currentFileIndex;
    
    /* Move to next file (all files in list are WAV files) */
    currentFileIndex = (currentFileIndex + 1) % fileCount;
    
    /* If same file was found (only one WAV file), restart from beginning of that file */
    if (currentFileIndex == startIndex) {
        printf("USBAudioPlayer: Only one WAV file, restarting from beginning\r\n");
    }
    
    /* Open next WAV file */
    char filePath[300];
    sprintf(filePath, "%s%s", SDPath, fileList[currentFileIndex]);
    
    FRESULT res = f_open(&currentFile, filePath, FA_READ);
    if (res == FR_OK) {
        fileOpen = true;
        currentFileType = FILE_WAV;
        playerState = PLAYER_PLAYING;  /* Maintain playing state */
        
        /* Parse WAV header */
        if (parse_wave_header(&currentFile, &WaveFormat) == 0) {
            /* Validate format - only 16-bit PCM supported */
            if (WaveFormat.BitsPerSample != 16) {
                /* Skip this file and try next */
                PlayNextWAVFile();
                return;
            }
            
            /* Reset WAV buffer state */
            wav_buffer_in_ptr = 0;
            wav_buffer_out_ptr = 0;
            wav_playback_started = false;
            frequencySetComplete = false;
            
            /* Pre-fill circular buffer with initial data */
            UINT bytesRead;
            FRESULT readRes = f_read(&currentFile, wav_circular_buffer, 
                                    WAV_BUFFER_SIZE, &bytesRead);
            if (readRes == FR_OK && bytesRead != 0) {
                wav_buffer_in_ptr = 0;
            } else {
                /* Failed to read - try next file */
                PlayNextWAVFile();
                return;
            }
        } else {
            /* Failed to parse header - try next file */
            PlayNextWAVFile();
            return;
        }
    } else {
        /* Failed to open file - try next file */
        PlayNextWAVFile();
        return;
    }
}


/**
  * @brief Start playing
  */
void USBAudioPlayer_Start(void)
{
    if (playerState == PLAYER_READY || playerState == PLAYER_PAUSED) {
        if (currentFileIndex >= fileCount) {
            currentFileIndex = 0; /* Wrap to beginning of list */
        }
        
        /* All files in list are WAV files - open first one */
        if (fileCount == 0) {
            printf("USBAudioPlayer: ERROR - No audio files found to play\r\n");
            playerState = PLAYER_ERROR;
            return;
        }
        
        /* Close previous file if open */
        if (fileOpen) {
            f_close(&currentFile);
        }
        
        /* Open selected audio file */
        char filePath[300];
        sprintf(filePath, "%s%s", SDPath, fileList[currentFileIndex]);
        
        FRESULT res = f_open(&currentFile, filePath, FA_READ);
        if (res == FR_OK) {
            playerState = PLAYER_PLAYING;
            fileOpen = true;
            currentFileType = FILE_WAV;
            
            printf("USBAudioPlayer: Starting WAV file: %s\r\n", fileList[currentFileIndex]);
            /* Parse WAV file header */
            if (parse_wave_header(&currentFile, &WaveFormat) == 0) {
                /* Validate format - only 16-bit PCM supported */
                if (WaveFormat.BitsPerSample != 16) {
                    printf("USBAudioPlayer: ERROR - Unsupported WAV bits per sample: %lu\r\n", WaveFormat.BitsPerSample);
                    playerState = PLAYER_ERROR;
                    return;
                }
                
                /* Reset WAV buffer state */
                wav_buffer_in_ptr = 0;
                wav_buffer_out_ptr = 0;
                wav_playback_started = false;
                
                /* Pre-fill circular buffer with initial data */
                UINT bytesRead;
                FRESULT res = f_read(&currentFile, wav_circular_buffer, 
                                    WAV_BUFFER_SIZE, &bytesRead);
                if (res == FR_OK && bytesRead != 0) {
                    wav_buffer_in_ptr = 0;  /* Write pointer starts at 0 after initial fill */
                } else {
                    printf("USBAudioPlayer: ERROR - Failed to read WAV initial data\r\n");
                    playerState = PLAYER_ERROR;
                }
            } else {
                printf("USBAudioPlayer: ERROR - Failed to parse WAV header\r\n");
                playerState = PLAYER_ERROR;
            }
        } else {
            printf("USBAudioPlayer: ERROR - Failed to open file: %s\r\n", filePath);
            playerState = PLAYER_ERROR;
        }
    }
}

/**
  * @brief Stop playing
  */
void USBAudioPlayer_Stop(void)
{
    if (playerState == PLAYER_PLAYING || playerState == PLAYER_PAUSED) {
        if (fileOpen) {
            f_close(&currentFile);
            fileOpen = false;
        }
        currentFileType = FILE_NONE;
        playerState = PLAYER_IDLE;
    }
}

/**
  * @brief Get current player state
  */
PlayerState USBAudioPlayer_GetState(void)
{
    return playerState;
}

/**
  * @brief Callback when USB audio frequency is set
  */
void USBAudioPlayer_FrequencySetCallback(void)
{
    /* Frequency set callback from USB Host Library */
    /* WAV handles frequency setting in USBAudioPlayer_Process polling logic */
    /* This callback is just for notification - don't set flags here */
}

/**
  * @brief Callback when USB audio buffer is empty
  */
void USBAudioPlayer_BufferEmptyCallback(void)
{
    /* Empty callback - buffer refill handled in USBAudioPlayer_Process polling */
}

