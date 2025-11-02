/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : USB_Host/HID_Standalone/Core/Src/main.c
  * @author         : MCD Application Team
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2019 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"
#include "usb_host.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usbh_audio.h"
#include "usb_audio_player.h"
#include <math.h>

extern USBH_HandleTypeDef hUsbHostHS;
extern ApplicationTypeDef Appli_state;

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

UART_HandleTypeDef UartHandle;

SD_HandleTypeDef hsd1;

/* USER CODE BEGIN PV */
#define SAMPLE_RATE 48000
#define TONE_FREQUENCY 440  // A4 note in Hz
#define BUFFER_SIZE (SAMPLE_RATE / 100)  // 10ms buffer
#define BITS_PER_SAMPLE 16
#define CHANNELS 2

uint8_t audio_buffer[BUFFER_SIZE * CHANNELS * (BITS_PER_SAMPLE / 8)];
uint32_t tone_phase = 0;
uint8_t test_tone_started = 0;
uint8_t frequency_set_complete = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_SDMMC1_SD_Init(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
void GenerateTestTone(uint8_t *buffer, uint32_t buffer_size);
void StartTestTone(void);
void ListFilesOnSD(void);
void USBAudioPlayer_FrequencySetCallback(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/**
  * @brief Generate a sine wave test tone
  * @param buffer: pointer to audio buffer
  * @param buffer_size: size of buffer in bytes
  */
void GenerateTestTone(uint8_t *buffer, uint32_t buffer_size)
{
  uint16_t *samples = (uint16_t *)buffer;
  uint32_t num_samples = buffer_size / (sizeof(uint16_t) * CHANNELS);
  
  // Phase increment per sample for the desired frequency
  static float phase_increment = 0;
  if (phase_increment == 0)
  {
    phase_increment = (2.0f * M_PI * TONE_FREQUENCY) / SAMPLE_RATE;
  }
  
  // Create rhythmic pattern: beep pattern
  // Each beep is 0.2 seconds (9600 samples at 48kHz)
  // Pattern: beep(on 0.2s) - silence(off 0.2s) - repeat
  uint32_t beep_duration = SAMPLE_RATE / 5;  // 0.2 seconds on
  uint32_t cycle_duration = beep_duration * 2;  // 0.4 seconds total cycle
  uint32_t position_in_cycle = tone_phase % cycle_duration;
  float envelope = 1.0f;
  
  // Apply envelope for rhythmic pattern
  if (position_in_cycle < beep_duration)
  {
    envelope = 1.0f;  // On (beep)
  }
  else
  {
    envelope = 0.0f;  // Off (silence)
  }
  
  for (uint32_t i = 0; i < num_samples; i++)
  {
    // Generate sine wave sample using accumulated phase
    float phase = tone_phase * phase_increment;
    float sample = sinf(phase);
    
    int16_t sample_value = (int16_t)(sample * 8000.0f * envelope);
    
    // Interleave stereo samples
    samples[i * CHANNELS] = sample_value;     // Left channel
    samples[i * CHANNELS + 1] = sample_value; // Right channel
    
    tone_phase++;
    // Wrap phase to prevent overflow (2π wraps naturally in sin())
    if (tone_phase >= 0xFFFFFFFF / 4)  // Reset before overflow
    {
      tone_phase = 0;
    }
  }
}

/**
  * @brief Start playing test tone when USB audio device is ready
  */
void StartTestTone(void)
{
  static uint8_t freq_set_attempted = 0;
  
  if (Appli_state == APPLICATION_READY && !test_tone_started)
  {
    if (!freq_set_attempted)
    {
      USBH_StatusTypeDef freq_status = USBH_AUDIO_SetFrequency(&hUsbHostHS, 
                                                                  SAMPLE_RATE, 
                                                                  CHANNELS, 
                                                                  BITS_PER_SAMPLE);
      freq_set_attempted = 1;
      frequency_set_complete = 0;
      
      if (freq_status == USBH_BUSY)
      {
        freq_set_attempted = 0;  // Retry on next loop
      }
    }
    
    if (frequency_set_complete && !test_tone_started)
    {
      tone_phase = 0;
      GenerateTestTone(audio_buffer, sizeof(audio_buffer));
      
      if (USBH_AUDIO_Play(&hUsbHostHS, audio_buffer, sizeof(audio_buffer)) == USBH_OK)
      {
        test_tone_started = 1;
        BSP_LED_On(LED1);
      }
      else
      {
        test_tone_started = 1;  // Prevent endless retries
      }
    }
  }
}

/**
  * @brief Callback called when audio buffer is empty (user needs to provide next buffer)
  * @param phost: USB Host handle
  */
void USBH_AUDIO_BufferEmptyCallback(USBH_HandleTypeDef *phost)
{
  UNUSED(phost);
  
  static uint32_t callback_count = 0;
  if ((callback_count++ % 100) == 0) {
    printf("BufferEmpty callback #%lu\r\n", callback_count);
  }
  
  // Use MP3 player instead of test tone
  USBAudioPlayer_BufferEmptyCallback();
  BSP_LED_Toggle(LED3);
}

/**
  * @brief Frequency set callback
  * @param phost: USB Host handle
  */
void USBH_AUDIO_FrequencySet(USBH_HandleTypeDef *phost)
{
  UNUSED(phost);
  printf("*** USBH_AUDIO_FrequencySet CALLBACK CALLED! ***\r\n");
  frequency_set_complete = 1;
  BSP_LED_On(LED2);
  
  // Notify audio player that frequency is set
  USBAudioPlayer_FrequencySetCallback();
}

/**
  * @brief List all files in the SD card root directory
  */
void ListFilesOnSD(void)
{
  DIR dir;
  FILINFO fno;
  FRESULT res;
  
  printf("\r\n=== SD Card File Listing ===\r\n");
  
  // Open root directory
  res = f_opendir(&dir, "/");
  if (res != FR_OK)
  {
    printf("Failed to open root directory. Error: %d\r\n", res);
    return;
  }
  
  uint32_t file_count = 0;
  
  // Read directory entries
  while (1)
  {
    res = f_readdir(&dir, &fno);
    if (res != FR_OK || fno.fname[0] == 0)
    {
      break;  // End of directory or error
    }
    
    if (fno.fname[0] == '.')
    {
      continue;  // Skip hidden files
    }
    
    file_count++;
    
    // Determine file type
    if (fno.fattrib & AM_DIR)
    {
      printf("[DIR ]  %s\r\n", fno.fname);
    }
    else
    {
      printf("[FILE]  %s  (%lu bytes)\r\n", fno.fname, fno.fsize);
    }
  }
  
  printf("\r\nTotal files/directories: %lu\r\n", file_count);
  printf("=== End of Listing ===\r\n\r\n");
  
  f_closedir(&dir);
}

/* USER CODE END 0 */

/* USER CODE BEGIN PFP */

#ifdef __GNUC__
  /* With GCC/RAISONANCE, small printf (option LD Linker->Libraries->Small printf
     set to 'Yes') calls __io_putchar() */
  #define UART_DISPLAY int __io_putchar(int ch)
#else
  #define UART_DISPLAY int fputc(int ch, FILE *f)
#endif /* __GNUC__ */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* Configure the application hardware resources */
  BSP_LED_Init(LED1);
  BSP_LED_Init(LED2);
  BSP_LED_Init(LED3);
  BSP_LED_Init(LED4);


  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
 
  HAL_PWREx_EnableUSBVoltageDetector();
  MX_USB_HOST_Init();
  MX_SDMMC1_SD_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */

  // Mount SD card and list files
  if (f_mount(&SDFatFS, SDPath, 1) == FR_OK)
  {
    printf("SD card mounted successfully!\r\n");
    HAL_Delay(100);
    ListFilesOnSD();
    
    // Initialize audio player
    USBAudioPlayer_Init();
  }
  else
  {
    printf("Failed to mount SD card!\r\n");
  }

  /* USER CODE END 2 */

  /* Infinite loop */

  while (1)
  {
  /* USER CODE BEGIN WHILE */
    MX_USB_HOST_Process();
    
    // Debug: Print USB Host state periodically and check audio state
    static uint32_t state_print_count = 0;
    static uint32_t last_gState = 0, last_EnumState = 0, last_Appli_state = 0;
    if (++state_print_count % 10000 == 0) {
        extern USBH_HandleTypeDef hUsbHostHS;
        
        // Only print when state changes
        if (hUsbHostHS.gState != last_gState || hUsbHostHS.EnumState != last_EnumState || Appli_state != last_Appli_state) {
            printf("USB Host State: gState=%d, EnumState=%d, Appli_state=%d\r\n", 
                   hUsbHostHS.gState, hUsbHostHS.EnumState, Appli_state);
            last_gState = hUsbHostHS.gState;
            last_EnumState = hUsbHostHS.EnumState;
            last_Appli_state = Appli_state;
        }
        
        // Debug: Check audio playback state - just print occasionally
        // The detailed state will be printed in the callbacks
    }
    
    // Process audio player when USB is ready
    if (Appli_state == APPLICATION_READY)
    {
      USBAudioPlayer_Process();
      
      // Auto-start when ready
      if (USBAudioPlayer_GetState() == PLAYER_READY)
      {
        USBAudioPlayer_Start();
        BSP_LED_On(LED1);
      }
    }
    
    /* USER CODE END WHILE */
  }

}

/**
  * @brief  System Clock Configuration
  *         The system Clock is configured as follow :
  *            System Clock source            = PLL (HSE)
  *            SYSCLK(Hz)                     = 280000000 (CPU Clock)
  *            HCLK(Hz)                       = 280000000 (Bus matrix and AHBs Clock)
  *            AHB Prescaler                  = 1
  *            CD APB3 Prescaler              = 2 (APB3 Clock  140MHz)
  *            CD APB1 Prescaler              = 2 (APB1 Clock  140MHz)
  *            CD APB2 Prescaler              = 2 (APB2 Clock  140MHz)
  *            SRD APB4 Prescaler             = 2 (APB4 Clock  140MHz)
  *            HSE Frequency(Hz)              = 24000000
  *            PLL_M                          = 12
  *            PLL_N                          = 280
  *            PLL_P                          = 2
  *            PLL_Q                          = 2
  *            PLL_R                          = 2
  *            VDD(V)                         = 3.3
  *            Flash Latency(WS)              = 6
  * @param  None
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  HAL_StatusTypeDef ret = HAL_OK;

  /* The voltage scaling allows optimizing the power consumption when the device is
  clocked below the maximum system frequency, to update the voltage scaling value
  regarding system frequency refer to product datasheet.
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /* Enable HSE Oscillator and activate PLL with HSE as source */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_OFF;
  RCC_OscInitStruct.CSIState = RCC_CSI_OFF;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;

  RCC_OscInitStruct.PLL.PLLM = 12;
  RCC_OscInitStruct.PLL.PLLN = 280;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;

  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_1;
  ret = HAL_RCC_OscConfig(&RCC_OscInitStruct);
  if(ret != HAL_OK)
  {
    Error_Handler();
  }

  /* Select PLL as system clock source and configure  bus clocks dividers */
  RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_PCLK1 | \
    RCC_CLOCKTYPE_PCLK2  | RCC_CLOCKTYPE_D3PCLK1);

  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
  ret = HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_6);
  if(ret != HAL_OK)
  {
    Error_Handler();
  }

    /*
  Note : The activation of the I/O Compensation Cell is recommended with communication  interfaces
          (GPIO, SPI, FMC, OSPI ...)  when  operating at  high frequencies(please refer to product datasheet)
          The I/O Compensation Cell activation  procedure requires :
        - The activation of the CSI clock
        - The activation of the SYSCFG clock
        - Enabling the I/O Compensation Cell : setting bit[0] of register SYSCFG_CCCSR

          To do this please uncomment the following code
  */

  // /*
  __HAL_RCC_CSI_ENABLE() ;

  __HAL_RCC_SYSCFG_CLK_ENABLE() ;

  HAL_EnableCompensationCell();
  // */
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
  static void MX_SDMMC1_SD_Init(void){
    hsd1.Instance = SDMMC1;
    hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd1.Init.ClockDiv = 2;  // Divide clock for stability (SD max 400kHz init)
    
    // Add delay to allow SD card to stabilize after power on
    HAL_Delay(200);
    
    if (HAL_SD_Init(&hsd1) != HAL_OK)
    {
      printf("SD Card HAL_SD_Init failed!\r\n");
      printf("SD Error Code: 0x%08lX\r\n", (unsigned long)hsd1.ErrorCode);
      // Don't call Error_Handler - continue without SD card
      return;
    }
}



static void MX_USART1_UART_Init(void)
{
  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */

  /*##-1- Configure the UART peripheral ######################################*/
  /* Put the USART peripheral in the Asynchronous mode (UART Mode) */
  /* UART configured as follows:
      - Word Length = 8 Bits (data size 8bits)
      - Stop Bit    = One Stop bit
      - Parity      = None
      - BaudRate    = 1152000 baud
      - Hardware flow control disabled (RTS and CTS signals) */

  UartHandle.Instance        = USARTx;
  UartHandle.Init.BaudRate   = 115200;
  UartHandle.Init.WordLength = UART_WORDLENGTH_8B;
  UartHandle.Init.StopBits   = UART_STOPBITS_1;
  UartHandle.Init.Parity     = UART_PARITY_NONE;
  UartHandle.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
  UartHandle.Init.Mode       = UART_MODE_TX_RX;
  UartHandle.Init.OverSampling = UART_OVERSAMPLING_16;

  if(HAL_UART_Init(&UartHandle) != HAL_OK)
  {
    /* Initialization Error */
    Error_Handler();
  }

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOI_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin : PF10 */
  GPIO_InitStruct.Pin = GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/**
  * @brief  Retargets the C library printf function to the USART.
  * @param  None
  * @retval None
  */
UART_DISPLAY
{
  /* Place your implementation of fputc here */
  /* e.g. write a character to the EVAL_COM1 and Loop until the end of transmission */
  HAL_UART_Transmit(&UartHandle, (uint8_t *)&ch, 1, 0xFFFF);

  return ch;
}
/* USER CODE END 4 */

/**
  * @brief  Configure the MPU attributes
  * @param  None
  * @retval None
  */
void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /* Configure the MPU as Strongly ordered for not defined regions */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.BaseAddress = 0x00;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32B;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Configure the MPU attributes as Normal Non Cacheable for SRAM1 */

  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.BaseAddress = 0x24000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  BSP_LED_On(LED2);
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     tex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
