/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2026 STMicroelectronics.
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
#include "crc.h"
#include "usart.h"
#include "gpio.h"
#include "trace.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum
{
  BL_GET_VER = 0x51,
  BL_GET_HELP,
  BL_GET_CID,
  BL_GET_RDP_STATUS,
  BL_GO_TO_ADDR = 0x55,
  BL_FLASH_ERASE = 0x56,
  BL_MEM_WRITE = 0x57,
  BL_EN_R_W_PROTECT,
  BL_MEM_READ,
  BL_READ_SECTOR_STATUS,
  BL_OTP_READ,
} enEvent;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BL_ACK 0xA5
#define BL_NACK 0x7F

#define ADDR_VALID 0x00
#define ADDR_INVALID 0x01

#define SRAM1_BASE 0x20000000
#define SRAM2_BASE 0x2001C000
#define SRAM1_SIZE 112 * 1024 // STM32F446RE has 112KB of SRAM1
#define SRAM1_END (SRAM1_BASE + SRAM1_SIZE)
#define SRAM2_SIZE 16 * 1024 // STM32F446RE has 16KB of SRAM2
#define SRAM2_END (SRAM2_BASE + SRAM2_SIZE)
#define BKPSRAM_BASE 0x40024000
#define FLASH_END 0x0807FFFF
#define BKPSRAM_SIZE 4 * 1024 // STM32F446RE has 4KB of SRAM2
#define BKPSRAM_END (BKPSRAM_BASE + BKPSRAM_SIZE)

#define FLASH_BASE_ADDR 0x08000000UL
#define FLASH_TOTAL_PAGES 64U
#define APP_START_PAGE 32U

#define INVALID_SECTOR 0x01
#define FLASH_ERASE_FAILED 0x02
#define FLASH_TYPEPROGRAM_BYTE 0x00000000

#define BL_RX_BUFFER_SIZE 512
#define TIME_WAIT 50

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
unsigned char blRxBuffer[BL_RX_BUFFER_SIZE] = {0};
// char pBuffer[512] = {0};
uint8_t blRxByte;
volatile uint16_t blRxArrIdx = 0;
volatile uint32_t blLastRxTick = 0;
volatile uint8_t blRxNewByteFlag = 0;
volatile uint8_t blRxCompleteFlag = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
void SystemClock_Config(void);
void bootloader_handle_go_cmd(uint8_t *pBuffer);
uint8_t execute_flash_erase(uint8_t sector_number, uint8_t number_of_sector);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
int _write(int file, char *ptr, int len)
{
  (void)file;
  HAL_UART_Transmit(&huart1, (uint8_t *)ptr, len, 1000);
  return len;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    if (blRxArrIdx < BL_RX_BUFFER_SIZE)
    {
      blRxBuffer[blRxArrIdx++] = blRxByte;

      blRxNewByteFlag = 1;
      blLastRxTick = HAL_GetTick();
    }

    HAL_UART_Receive_IT(&huart2, &blRxByte, 1);
  }
}

void bootloader_uart_start_receive(void)
{
  blRxArrIdx = 0;
  blRxNewByteFlag = 0;
  blRxCompleteFlag = 0;

  HAL_UART_Receive_IT(&huart2, &blRxByte, 1);
}

uint8_t bootloader_uart_check_rx(void)
{
  if (blRxNewByteFlag == 0)
  {
    return 0;
  }

  if (blRxCompleteFlag == 0)
  {
    if ((HAL_GetTick() - blLastRxTick) >= TIME_WAIT)
    {
      blRxCompleteFlag = 1;
    }
  }

  return blRxCompleteFlag;
}

void jump_to_application(uint32_t address)
{
  void (*app_reset_handler)(void);
  printf("BL_DEBUG: bootloader jump to user app\r\n");

  /* �?�?c 4 byte đầu tiên của vector table */
  uint32_t msp_value = *(uint32_t *)address;
  printf("BL_DEBUG: MSP value: %08lX\r\n", msp_value);

  /* Thiết lạp MSP */
  __set_MSP(msp_value);

  /*Lấy địa chỉ resethandle từ 4 byte tiếp theo*/
  uint32_t resethandler_address = *(uint32_t *)(address + 4);

  /* Gán địa chỉ Reset Handler vào con tr�? hàm */
  app_reset_handler = (void *)resethandler_address;

  /* G�?i Reset Handler để chuyển quy�?n thực thi từ Bootloader sang Application */
  app_reset_handler();
}

void bootloader_uart_read_data(void)
{
  bootloader_uart_start_receive();

  while (1)
  {
    if (bootloader_uart_check_rx())
    {
      printf("BL_DEBUG: recieve complete\r\n");

      trace_hex("Rx arr", blRxBuffer, blRxArrIdx);

      switch (blRxBuffer[1])
      {
      case BL_GET_VER:
        bootloader_handle_getver_cmd(blRxBuffer);
        break;

      case BL_GO_TO_ADDR:
        bootloader_handle_go_cmd(blRxBuffer);
        break;

      case BL_FLASH_ERASE:
        bootloader_handle_flash_erase_cmd(blRxBuffer);
        break;

      case BL_MEM_WRITE:
        bootloader_handle_mem_write_cmd(blRxBuffer);
        break;

      default:
        break;
      }

      memset(blRxBuffer, 0, BL_RX_BUFFER_SIZE);

      blRxArrIdx = 0;
      blRxNewByteFlag = 0;
      blRxCompleteFlag = 0;
    }
  }
}

uint8_t bootloader_verify_crc(uint8_t *pData, uint32_t len, uint32_t crc_host)
{
  // uint32_t uwCRCValue = 0xff;
  // for (uint32_t i = 0; i < len; i++)
  // {
  //   uint32_t i_data = pData[i];
  //   uwCRCValue = HAL_CRC_Accumulate(&hcrc, &i_data, 1);
  // }

  // trace("crc: %08X", uwCRCValue);

  // if (uwCRCValue == crc_host)
  // {
  //   // return VERIFY_CRC_SUCCESS;
  //   return 0;
  // }

  return 0;
}

void bootloader_send_ack(uint8_t command_code, uint8_t follow_len)
{
  uint8_t ack_buf[2];
  ack_buf[0] = BL_ACK;
  ack_buf[1] = follow_len;
  HAL_UART_Transmit(&huart2, ack_buf, 2, HAL_MAX_DELAY);
}

void bootloader_uart_write_data(uint8_t *data, uint16_t len)
{
  HAL_UART_Transmit(&huart2, data, len, 1000);
}

void bootloader_send_nack(void)
{
  uint8_t nack = BL_NACK;
  HAL_UART_Transmit(&huart2, &nack, 1, HAL_MAX_DELAY);
}

uint8_t verify_address(uint32_t go_address)
{
  if (go_address >= SRAM1_BASE && go_address <= SRAM1_END)
  {

    return ADDR_VALID;
  }
  else if (go_address >= SRAM2_BASE && go_address <= SRAM2_END)
  {

    return ADDR_VALID;
  }
  else if (go_address >= FLASH_BASE && go_address <= FLASH_END)
  {

    return ADDR_VALID;
  }
  else if (go_address >= BKPSRAM_BASE && go_address <= BKPSRAM_END)
  {

    return ADDR_VALID;
  }
  else
    return ADDR_INVALID;
}

void bootloader_handle_getver_cmd(uint8_t *blRxBuffer)
{
  uint8_t bl_version;
  printf("BL_DEBUG: bootloader get version\r\n");

  uint32_t command_packet_len = blRxBuffer[0] + 1;
  uint32_t host_crc = *((uint32_t *)(blRxBuffer + command_packet_len - 4));

  if (!bootloader_verify_crc(&blRxBuffer[0], command_packet_len - 4, host_crc))
  {
    printf("BL_DEBUG: checksum success\r\n");

    bootloader_send_ack(blRxBuffer[0], 1);

    bl_version = 0x10;
    printf("BL_DEBUG: BL_VER: %d\r\n", bl_version);

    bootloader_uart_write_data(&bl_version, 1);
  }
  else
  {
    printf("BL_DEBUG: checksum fail\r\n");
    bootloader_send_nack();
  }
}

void bootloader_handle_go_cmd(uint8_t *pBuffer)
{
  uint32_t go_address = 0;
  uint8_t addr_valid = ADDR_VALID;
  uint8_t addr_invalid = ADDR_INVALID;

  printf("BL_DEBUG: bootloader go to address\r\n");

  uint32_t command_packet_len = pBuffer[0] + 1;
  uint32_t host_crc = *((uint32_t *)(pBuffer + command_packet_len - 4));

  if (!bootloader_verify_crc(&pBuffer[0], command_packet_len - 4, host_crc))
  {
    printf("BL_DEBUG: checksum success\r\n");

    bootloader_send_ack(pBuffer[0], 1);

    go_address = *((uint32_t *)&pBuffer[2]);
    printf("BL_DEBUG: Go address: %08lX\r\n", go_address);

    if (verify_address(go_address) == ADDR_VALID)
    {
      bootloader_uart_write_data(&addr_valid, 1);
      go_address |= 1;

      void (*lets_jump)(void) = (void *)go_address;
      printf("BL_DEBUG: jumping to go address\r\n");
      lets_jump();
    }
    else
    {
      printf("BL_DEBUG: Go addr invalid\r\n");
      bootloader_uart_write_data(&addr_invalid, 1);
    }
  }
  else
  {
    printf("BL_DEBUG: checksum fail\r\n");
    bootloader_send_nack();
  }
}

void bootloader_handle_flash_erase_cmd(uint8_t *pBuffer)
{
  uint8_t erase_status = 0x00;
  printf("BL_DEBUG: bootloader flash erase\r\n");

  /*Tính tổng độ dài gói lệnh, bao gồm cả byte độ dài*/
  uint32_t command_packet_len = blRxBuffer[0] + 1;
  uint32_t host_crc = *((uint32_t *)(pBuffer + command_packet_len - 4));

  /*Check crc*/
  if (!bootloader_verify_crc(&pBuffer[0], command_packet_len - 4, host_crc))
  {
    printf("BL_DEBUG: checksum success\r\n");
    bootloader_send_ack(pBuffer[0], 1);
    printf("BL_DEBUG: initial sector: %d no of sector: %d\r\n", pBuffer[2], pBuffer[3]);

    erase_status = execute_flash_erase(pBuffer[2], pBuffer[3]);

    printf("BL_DEBUG: flash erase status: %d\r\n", erase_status);

    bootloader_uart_write_data(&erase_status, 1);
  }
  else
  {
    printf("BL_DEBUG: cheksum fail\r\n");
    bootloader_send_nack();
  }
}

uint8_t execute_flash_erase(uint8_t page_number,
                            uint8_t number_of_pages)
{
  FLASH_EraseInitTypeDef eraseInit = {0};
  uint32_t pageError = 0;
  HAL_StatusTypeDef status;

  status = HAL_FLASH_Unlock();

  if (status != HAL_OK)
  {
    return FLASH_ERASE_FAILED;
  }

  /* Kiểm tra yêu cầu Mass Erase */
  if (page_number == 0xFF)
  {
    eraseInit.TypeErase = FLASH_TYPEERASE_MASSERASE;
  }
  else
  {
    /* Kiểm tra page */
    if ((number_of_pages == 0) ||
        (page_number < APP_START_PAGE) ||
        (page_number >= FLASH_TOTAL_PAGES) ||
        (number_of_pages > FLASH_TOTAL_PAGES - page_number))
    {
      HAL_FLASH_Lock();
      return INVALID_SECTOR;
    }

    /* Cấu hình Page Erase */
    eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInit.PageAddress = FLASH_BASE_ADDR +
                            (page_number * PAGESIZE);
    eraseInit.NbPages = number_of_pages;
  }

  /* Xóa Flash */
  status = HAL_FLASHEx_Erase(&eraseInit, &pageError);

  HAL_FLASH_Lock();

  if (status != HAL_OK)
  {
    printf("BL_DEBUG: HAL erase error: %d\r\n", status);
    printf("BL_DEBUG: Page error: %08lX\r\n", pageError);

    return FLASH_ERASE_FAILED;
  }

  return HAL_OK;
}

uint8_t execute_mem_write(uint8_t *pBuffer, uint32_t mem_address, uint32_t len)
{
  uint8_t status = HAL_OK;

  HAL_FLASH_Unlock();

  /* Ghi theo Half-Word (16-bit) - Đảm bảo len là số chẵn */
  for (uint32_t i = 0; i < len; i += 2)
  {
    // Lấy 2 byte từ pBuffer ghép lại thành 16-bit data
    uint16_t data = *((uint16_t *)(&pBuffer[i]));

    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, mem_address + i, (uint64_t)data);

    if (status != HAL_OK)
    {
      printf("BL_DEBUG: Flash write error at address: %08lX\r\n", mem_address + i);
      break;
    }
  }

  HAL_FLASH_Lock();

  return status;
}

void bootloader_handle_mem_write_cmd(uint8_t *pBuffer)
{
  uint8_t write_status = 0x00;

  uint8_t payload_len = pBuffer[6];
  uint32_t mem_address = *((uint32_t *)(&pBuffer[2]));

  printf("BL_DEBUG: bootloader mem write\r\n");

  /*Tính tổng độ dài gói lệnh, bao gồm cả byte độ dài*/
  uint32_t command_packet_len = blRxBuffer[0] + 1;
  uint32_t host_crc = *((uint32_t *)(pBuffer + command_packet_len - 4));

  if (!bootloader_verify_crc(&pBuffer[0], command_packet_len - 4, host_crc))
  {
    printf("BL_DEBUG: checksum success\r\n");
    bootloader_send_ack(pBuffer[0], 1);
    printf("BL_DEBUG: mem write address: %08lX\r\n", mem_address);

    /*Kiểm tra địa chỉ bộ nhớ có hợp lệ không*/
    if (verify_address(mem_address) == ADDR_VALID)
    {
      printf("BL_DEBUG: valid mem write address\r\n");
      /*Ghi dữ liệu vào flash, abwts đầu từ vị trí thứ pBuffer[7] (payload)*/
      write_status = execute_mem_write(&pBuffer[7], mem_address, payload_len);
      bootloader_uart_write_data(&write_status, 1);
    }
    else
    {
      printf("BL_DEBUG: invalid mem write address\r\n");
      write_status = ADDR_INVALID;
      bootloader_uart_write_data(&write_status, 1);
    }
  }
  else
  {
    printf("BL_DEBUG: checksum fail\r\n");
    bootloader_send_nack();
  }
}

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_CRC_Init();
  /* USER CODE BEGIN 2 */
  printf("Hello\r\n");
  HAL_UART_Transmit(&huart2, (uint8_t *)"UART2\r\n", strlen("UART2\r\n"), 1000);

  if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_RESET)
  // if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_SET)
  // if (1)
  {
    printf("BL_DEBUG: Button is pressed...going to BL mode\r\n");
    bootloader_uart_read_data();
  }
  else
  {
    printf("BL_DEBUG: Button is not pressed...executing user app\r\n");
    // bootloader_jump_to_user_app();
    jump_to_application(0x08008000);
  }

  // void bootloader_uart_read_data(void)
  // {
  // }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    HAL_Delay(100);
    // HAL_IWDG_Refresh(&hiwdg);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
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
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
