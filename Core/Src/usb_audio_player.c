/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usb_audio_player.c
  * @brief          : USB Audio Player for MP3 and WAV playback from SD card
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

/* MP3 decoder instance */
static HMP3Decoder hMP3Decoder = NULL;

/* Current file being played */
static FIL currentFile;
static char fileList[30][256];
static uint8_t fileCount = 0;
static uint8_t currentFileIndex = 0;
static FileType currentFileType = FILE_NONE;

/* Audio buffers */
AudioBuffer DecoderAudioBuffer;
static MP3FrameInfo frameInfo;
static WAV_FMT WaveFormat;
static uint8_t mp3Buffer[MAINBUF_SIZE];
static int mp3BytesLeft = 0;
static uint8_t *mp3ReadPtr = NULL;

/* Player state */
static PlayerState playerState = PLAYER_IDLE;
static bool needInitDecoder = false;
static bool bufferReady = false;
static bool fileOpen = false;
static bool frequencySetComplete = false;

/* MP3 frame chunk tracking for streaming in 10ms chunks */
static uint32_t currentFrameSampleOffset = 0;
static int16_t *currentFrameBuffer = NULL;
static int16_t *currentUSBBuffer = NULL;  /* Buffer currently being sent to USB */
static int currentActiveBuffer = 1;  /* Active buffer: 1 = buffer1, 2 = buffer2 */

/* Buffer flags */
static volatile uint8_t bbBUFFER_FULL_FLAG = 0;
static volatile uint8_t bbBUFFER2_FULL_FLAG = 0;

/* WAV playback circular buffer tracking */
#define WAV_BUFFER_SIZE (512 * 33)  /* 16896 bytes - matches reference implementation */
static uint8_t wav_circular_buffer[WAV_BUFFER_SIZE];
static uint32_t wav_buffer_in_ptr = 0;   /* Write position in circular buffer */
static uint32_t wav_buffer_out_ptr = 0;  /* Reserved for future use */
static bool wav_playback_started = false;

/* Access to audio buffer from main.c */
extern uint8_t audio_buffer[];

/* Forward declarations */
static void PlayNextWAVFile(void);

/**
  * @brief Find audio files (MP3 and WAV) on SD card
  * @note Finds both MP3 and WAV files, but currently only WAV files are played
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
                if (strcasecmp(ext, ".mp3") == 0 || 
                    strcasecmp(ext, ".wav") == 0 ||
                    strcasecmp(ext, ".MP3") == 0 || 
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
  * @brief Detect file type from extension
  */
static FileType DetectFileType(const char *filename)
{
    char *ext = strrchr(filename, '.');
    if (ext != NULL) {
        if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".MP3") == 0) {
            return FILE_MP3;
        } else if (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".WAV") == 0) {
            return FILE_WAV;
        }
    }
    return FILE_NONE;
}

/**
  * @brief Initialize the decoder for current file
  */
static void InitDecoder(void)
{
    if (!needInitDecoder) return;
    needInitDecoder = false;
    
    /* Free previous decoder instance */
    if (hMP3Decoder != NULL) {
        MP3FreeDecoder(hMP3Decoder);
    }
    
    /* Initialize new decoder instance */
    hMP3Decoder = MP3InitDecoder();
    
    /* Read initial data from file */
    UINT bytesRead;
    f_lseek(&currentFile, 0);
    f_read(&currentFile, mp3Buffer, MAINBUF_SIZE, &bytesRead);
    mp3BytesLeft = bytesRead;
    mp3ReadPtr = mp3Buffer;
    
    /* Zero-pad buffer if not enough data read */
    if (mp3BytesLeft < MAINBUF_SIZE) {
        memset(mp3Buffer + mp3BytesLeft, 0, MAINBUF_SIZE - mp3BytesLeft);
    }
}

/**
  * @brief Find MP3 sync word
  */
static int FindSyncWord(void)
{
    int offset = MP3FindSyncWord(mp3ReadPtr, mp3BytesLeft);
    if (offset >= 0) {
        mp3ReadPtr += offset;
        mp3BytesLeft -= offset;
    }
    return offset;
}

/**
  * @brief Get frame info
  */
static int GetFrameInfo(void)
{
    int offset = MP3GetNextFrameInfo(hMP3Decoder, &frameInfo, mp3ReadPtr);
    return offset;
}

/**
  * @brief Decode one MP3 frame to PCM
  */
static bool DecodeMP3Frame(void)
{
    /* Select target buffer - alternate between buffers for double buffering */
    int16_t *pcmBuffer;
    if (currentActiveBuffer == 1) {
        pcmBuffer = DecoderAudioBuffer.MP3Buffer.PCMbuffer1;
    } else {
        pcmBuffer = DecoderAudioBuffer.MP3Buffer.PCMbuffer2;
    }
    
    /* Decode MP3 frame to PCM */
    int err = MP3Decode(hMP3Decoder, &mp3ReadPtr, &mp3BytesLeft, pcmBuffer, 0);
    
    if (err) {
        return false;
    }
    
    /* Mark buffer as full and switch active buffer for next decode */
    bbBUFFER_FULL_FLAG = 1;
    currentActiveBuffer = (currentActiveBuffer == 1) ? 2 : 1;
    
    return true;
}

/**
  * @brief Read more data from file
  */
static void ReadMoreData(void)
{
    /* Move remaining data to start of buffer */
    if (mp3BytesLeft > 0) {
        memmove(mp3Buffer, mp3ReadPtr, mp3BytesLeft);
    }
    
    /* Read more data from file */
    UINT bytesRead;
    FRESULT res = f_read(&currentFile, mp3Buffer + mp3BytesLeft, 
                         MAINBUF_SIZE - mp3BytesLeft, &bytesRead);
    
    if (res == FR_OK && bytesRead > 0) {
        mp3BytesLeft += bytesRead;
        
        /* Zero-pad buffer if at end of file */
        if (bytesRead < (MAINBUF_SIZE - mp3BytesLeft)) {
            memset(mp3Buffer + mp3BytesLeft, 0, 
                   MAINBUF_SIZE - mp3BytesLeft);
        }
    }
    
    mp3ReadPtr = mp3Buffer;
}

/**
  * @brief Get pointer to next USB buffer chunk - uses decoder buffer directly
  * @param buf_ptr: Output pointer to buffer data
  * @param buf_len: Output buffer length in bytes
  * @return true if buffer available, false if need more data
  */
static bool GetUSBBufferChunk(uint8_t **buf_ptr, uint32_t *buf_len)
{
    /* Send silence if no decoded data available */
    if (!bbBUFFER_FULL_FLAG || currentUSBBuffer == NULL) {
        memset(audio_buffer, 0, 1920);
        *buf_ptr = audio_buffer;
        *buf_len = 1920;
        return true;
    }
    
    /* Calculate samples to send (480 samples = 10ms @ 48kHz) */
    uint32_t samplesNeeded = 480;
    uint32_t remainingFromFrame = frameInfo.outputSamps - currentFrameSampleOffset;
    uint32_t samplesToSend = (remainingFromFrame > samplesNeeded) ? samplesNeeded : remainingFromFrame;
    
    /* Check if frame will be exhausted after this chunk */
    if (currentFrameSampleOffset + samplesToSend >= frameInfo.outputSamps) {
        bbBUFFER_FULL_FLAG = 0;
    }
    
    /* Use decoder buffer directly for stereo (already interleaved L/R) */
    if (frameInfo.nChans == 2) {
        *buf_ptr = (uint8_t *)&currentUSBBuffer[currentFrameSampleOffset];
        *buf_len = samplesToSend * sizeof(int16_t);
        currentFrameSampleOffset += samplesToSend;
        
        if (!bbBUFFER_FULL_FLAG) {
            /* Frame exhausted, reset for next frame */
            currentUSBBuffer = NULL;
            currentFrameSampleOffset = 0;
        }
        
        return true;
    } else {
        /* Mono: convert to stereo by duplicating channel */
        int16_t *usbBuffer = (int16_t *)audio_buffer;
        for (uint32_t i = 0; i < samplesToSend; i++) {
            int16_t sample = currentUSBBuffer[currentFrameSampleOffset + i];
            usbBuffer[i * 2] = sample;     /* Left channel */
            usbBuffer[i * 2 + 1] = sample; /* Right channel */
        }
        
        /* Fill remaining buffer with silence if needed */
        if (samplesToSend < samplesNeeded) {
            memset(&usbBuffer[samplesToSend * 2], 0, (samplesNeeded - samplesToSend) * 2 * sizeof(int16_t));
        }
        
        currentFrameSampleOffset += samplesToSend;
        *buf_ptr = (uint8_t *)usbBuffer;
        *buf_len = 1920;
        
        if (!bbBUFFER_FULL_FLAG) {
            currentUSBBuffer = NULL;
            currentFrameSampleOffset = 0;
        }
        
        return true;
    }
}

/**
  * @brief Initialize the audio player
  */
void USBAudioPlayer_Init(void)
{
    /* Scan SD card for audio files */
    FindAudioFiles();
    
    if (fileCount == 0) {
        playerState = PLAYER_ERROR;
        return;
    }
    
    /* Initialize player state */
    currentFileIndex = 0;
    playerState = PLAYER_READY;
    needInitDecoder = true;
    fileOpen = false;
    
    /* Clear buffer flags */
    bbBUFFER_FULL_FLAG = 0;
    bbBUFFER2_FULL_FLAG = 0;
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
    } else if (currentFileType == FILE_MP3) {
        /* Initialize MP3 decoder if needed */
        if (needInitDecoder) {
            InitDecoder();
            
            /* Find MP3 sync word */
            if (FindSyncWord() < 0) {
                playerState = PLAYER_ERROR;
                return;
            }
            
            /* Get frame info */
            int frameOffset = GetFrameInfo();
            if (frameOffset < 0) {
                playerState = PLAYER_ERROR;
                return;
            }
            
            mp3ReadPtr += frameOffset;
            mp3BytesLeft -= frameOffset;
            
            /* Set USB audio frequency based on MP3 frame info */
            USBH_AUDIO_SetFrequency(&hUsbHostHS, frameInfo.samprate, 
                                    frameInfo.nChans, 16);
            
            /* Initialize decoder state */
            bufferReady = false;
            needInitDecoder = false;
            frequencySetComplete = false;
            
            /* Decode first 2 frames immediately to fill USB buffer */
            if (DecodeMP3Frame()) {
                bbBUFFER_FULL_FLAG = 1;
                
                /* Decode second frame if possible */
                if (!DecodeMP3Frame()) {
                    ReadMoreData();
                }
            }
        }
        
        /* Decode more frames when buffer is empty and frequency is set */
        if (frequencySetComplete && !bbBUFFER_FULL_FLAG) {
            if (DecodeMP3Frame()) {
                bbBUFFER_FULL_FLAG = 1;
            } else {
                /* Decode error or need more data - read more from file */
                if (mp3BytesLeft < 1024) {
                    ReadMoreData();
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
    /* Close current file */
    if (fileOpen) {
        f_close(&currentFile);
        fileOpen = false;
    }
    
    /* Reset WAV playback state */
    wav_playback_started = false;
    frequencySetComplete = false;
    wav_buffer_in_ptr = 0;
    wav_buffer_out_ptr = 0;
    
    /* Find next WAV file */
    uint8_t startIndex = currentFileIndex;
    bool found = false;
    
    do {
        currentFileIndex = (currentFileIndex + 1) % fileCount;
        
        if (DetectFileType(fileList[currentFileIndex]) == FILE_WAV) {
            found = true;
            break;
        }
    } while (currentFileIndex != startIndex);
    
    /* If no WAV file found, stop playback */
    if (!found) {
        playerState = PLAYER_IDLE;
        return;
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
            
            needInitDecoder = false;
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
  * @note Currently configured to play only WAV files (MP3 code preserved for future use)
  */
void USBAudioPlayer_Start(void)
{
    if (playerState == PLAYER_READY || playerState == PLAYER_PAUSED) {
        if (currentFileIndex >= fileCount) {
            currentFileIndex = 0; /* Wrap to beginning of list */
        }
        
        /* Find first WAV file in list - only WAV files are played */
        bool found_wav = false;
        if (fileCount > 0) {
            for (uint8_t i = 0; i < fileCount; i++) {
                if (DetectFileType(fileList[i]) == FILE_WAV) {
                    currentFileIndex = i;
                    found_wav = true;
                    break;
                }
            }
        }
        
        /* Only proceed if WAV file found */
        if (!found_wav) {
            playerState = PLAYER_ERROR;
            return;
        }
        
        /* Close previous file if open */
        if (fileOpen) {
            f_close(&currentFile);
        }
        
        /* Open selected WAV file */
        char filePath[300];
        sprintf(filePath, "%s%s", SDPath, fileList[currentFileIndex]);
        
        FRESULT res = f_open(&currentFile, filePath, FA_READ);
        if (res == FR_OK) {
            playerState = PLAYER_PLAYING;
            fileOpen = true;
            
            /* Verify file type is WAV (should always be true due to selection above) */
            currentFileType = DetectFileType(fileList[currentFileIndex]);
            
            if (currentFileType == FILE_WAV) {
                /* Parse WAV file header */
                if (parse_wave_header(&currentFile, &WaveFormat) == 0) {
                    /* Validate format - only 16-bit PCM supported */
                    if (WaveFormat.BitsPerSample != 16) {
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
                        playerState = PLAYER_ERROR;
                    }
                    
                    needInitDecoder = false;
                } else {
                    playerState = PLAYER_ERROR;
                }
            } else {
                /* Non-WAV file detected - should not happen due to selection above */
                /* MP3 code preserved but not used in current configuration */
                playerState = PLAYER_ERROR;
            }
        } else {
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
    /* For MP3 files, mark frequency set as complete */
    /* For WAV files, frequency and playback are handled in polling logic */
    if (currentFileType != FILE_WAV) {
        frequencySetComplete = true;
    }
    
    /* WAV files: playback initialization handled in USBAudioPlayer_Process */
    if (playerState == PLAYER_PLAYING && currentFileType == FILE_WAV && wav_playback_started == false) {
        return;
    }
}

/**
  * @brief Callback when USB audio buffer is empty
  */
void USBAudioPlayer_BufferEmptyCallback(void)
{
    /* Toggle LED indicator */
    BSP_LED_Toggle(LED3);
    
    if (playerState != PLAYER_PLAYING) {
        return;
    }
    
    if (currentFileType == FILE_MP3) {
        /* Setup buffer pointer for new frame if decoded data available */
        if (bbBUFFER_FULL_FLAG && currentUSBBuffer == NULL) {
            currentUSBBuffer = DecoderAudioBuffer.MP3Buffer.PCMbuffer1;
            currentFrameSampleOffset = 0;
            currentFrameBuffer = currentUSBBuffer;
        }
        
        /* Decode next frame if current frame exhausted */
        if (!bbBUFFER_FULL_FLAG && mp3BytesLeft > 1024) {
            if (DecodeMP3Frame()) {
                /* Point to newly decoded buffer (switch active buffer) */
                int newlyDecodedBuffer = (currentActiveBuffer == 1) ? 2 : 1;
                if (newlyDecodedBuffer == 1) {
                    currentUSBBuffer = DecoderAudioBuffer.MP3Buffer.PCMbuffer1;
                } else {
                    currentUSBBuffer = DecoderAudioBuffer.MP3Buffer.PCMbuffer2;
                }
                currentFrameSampleOffset = 0;
            } else {
                if (mp3BytesLeft < 1024) {
                    ReadMoreData();
                }
            }
        } else if (mp3BytesLeft < 1024) {
            ReadMoreData();
        }
        
        /* Send next audio chunk to USB */
        uint8_t *buf_ptr;
        uint32_t buf_len;
        if (GetUSBBufferChunk(&buf_ptr, &buf_len)) {
            USBH_AUDIO_Play(&hUsbHostHS, buf_ptr, buf_len);
        }
    } else if (currentFileType == FILE_WAV) {
        /* WAV files use circular buffer with polling-based refill */
        /* Refill handled in USBAudioPlayer_Process, not in callback */
    }
}

