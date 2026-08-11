/**
  ******************************************************************************
  * @file    STM32F4xx_IAP/src/menu.c
  * @author  MCD Application Team
  * @version V1.0.0
  * @date    10-October-2011
  * @brief   This file provides the software which contains the main menu routine.
  *          The main menu gives the options of:
  *             - downloading a new binary file,
  *             - uploading internal flash memory,
  *             - executing the binary file already loaded
  *             - disabling the write protection of the Flash sectors where the
  *               user loads his binary file.
  ******************************************************************************
  * @attention
  *
  * THE PRESENT FIRMWARE WHICH IS FOR GUIDANCE ONLY AIMS AT PROVIDING CUSTOMERS
  * WITH CODING INFORMATION REGARDING THEIR PRODUCTS IN ORDER FOR THEM TO SAVE
  * TIME. AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY
  * DIRECT, INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING
  * FROM THE CONTENT OF SUCH FIRMWARE AND/OR THE USE MADE BY CUSTOMERS OF THE
  * CODING INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
  *
  * <h2><center>&copy; COPYRIGHT 2011 STMicroelectronics</center></h2>
  ******************************************************************************
  */

/** @addtogroup STM32F4xx_IAP
  * @{
  */

/* Includes ------------------------------------------------------------------*/
#include "common.h"
#include "flash_if.h"
#include "menu.h"
#include "ymodem.h"
#include "delay.h"
#include "KT6328.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
pFunction Jump_To_Application;
uint32_t JumpAddress;
__IO uint32_t FlashProtection = 0;
uint8_t tab_1024[1024] =
  {
    0
  };
uint8_t FileName[FILE_NAME_LENGTH];

/* Private function prototypes -----------------------------------------------*/
void SerialDownload(void);
void SerialUpload(void);

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Jump to the user application
  * @param  None
  * @retval None
  */
void Execute_New_Program(void)
{
  const uint32_t app_sp = *(__IO uint32_t*)APPLICATION_ADDRESS;
  const uint32_t app_pc = *(__IO uint32_t*)(APPLICATION_ADDRESS + 4);

  /* ---- 检查是否合法（合法范围 0x20000000 ~ 0x20020000 / 0x08000000+） ---- */
  /*   不合法直接打印 ERR + 返回，不做死循环跳转  */
  if (((app_sp & 0xFFFE0000U) != 0x20000000U) || (app_sp == 0xFFFFFFFFU) ||
      ((app_pc & 0xFFFC0000U) != 0x08000000U) || (app_pc == 0xFFFFFFFFU)) {
    SerialPutString("[Jump] ERR: SP/PC invalid\r\n");
    return;
  }

  /* ============================================================
   * 方案：【关闭 BLE + 直接跳转 APP】
   *
   * 冷启动 KEY1 进 BootLoader 后按 3，也是在这里跳进 APP。
   * 之前用 NVIC_SystemReset() 软复位在这块板上表现为黑屏（软复位瞬间
   * PA3 电源锁存被释放，PMIC 可能直接断电，或复位后 LCD/外设状态异常）。
   * APP V3.6.0 的 BootLoaderJumpedPeriphCleanup() 在 main() 最前面
   * 专门清理 BootLoader 跳转留下的 USART/SPI/DMA/NVIC 脏状态，
   * 所以这里恢复 SecureCRT 时代可用的“直接跳转”，并额外把 KT6368A
   * 关掉再跳，APP 启动窗口不会再有 BLE 字节。
   * ============================================================ */

  // 1) 关 BLE 模块（PA8 拉低）
  KT6328_Disable();
  delay_ms(200);

  // 2) 清 UART 错误标志 + 排干 RX FIFO（防止 BootLoader 复位后仍有残留字节）
  {
    volatile uint32_t dummy;
    int j;
    for (j = 0; j < 64; j++) {
      dummy = USART1->SR;
      if ((dummy & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) == 0U) break;
      dummy = USART1->DR;
      (void)dummy;
    }
  }

  // 3) 关闭所有中断、关 SysTick、同步
  __disable_irq();
  SysTick->CTRL = 0X00;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;
  __DSB();
  __ISB();

  // 4) 清中断挂起位 + Flash 指令/数据缓存，保证新 APP 从干净状态启动
  NVIC->ICPR[0] = 0xFFFFFFFFU;
  NVIC->ICPR[1] = 0xFFFFFFFFU;
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
  FLASH->ACR |= (FLASH_ACR_ICRST | FLASH_ACR_DCRST);
  FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
  __DSB();
  __ISB();
  __set_CONTROL(0U);
  __ISB();
  __set_PSP(0U);

  // 5) 直接跳转 APP（APP 入口的 BootLoaderJumpedPeriphCleanup 会清理外设）
  JumpAddress = *(__IO uint32_t*) (APPLICATION_ADDRESS + 4);
  Jump_To_Application = (pFunction) JumpAddress;
  __set_MSP(app_sp);
  Jump_To_Application();

  /* 永远不会执行到这里 */
  while (1) {}
}

/**
  * @brief  Download a file via serial port
  * @param  None
  * @retval None
  */
void SerialDownload(void)
{
  uint8_t Number[10] = "          ";
  int32_t Size = 0;

  // user operation
  // clear the flag in flash in 0x08008000
  uint32_t flashdestination = ADDR_FLASH_SECTOR_2;
  // sector 2
  FLASH_If_Erase_One_Sector(2U);

  /* Waiting for file sent */
  SerialPutString("Waiting for the file to be sent ... (press 'a' to abort)\n\r");
  delay_ms(20);           /* let CR/LF from the menu key arrive */
  Serial_Flush_RX();      /* discard CR/LF so Ymodem starts clean */
  Size = Ymodem_Receive(&tab_1024[0]);
  if (Size > 0)
  {
    SerialPutString("\n\n\r Programming Completed Successfully!\n\r--------------------------------\r\n Name: ");
    SerialPutString(FileName);
    Int2Str(Number, Size);
    SerialPutString("\n\r Size: ");
    SerialPutString(Number);
    SerialPutString(" Bytes\r\n");
    SerialPutString("-------------------\n");

    // user operation
    // set the flag in flash in 0x08008000
    const char *str_flag = "APP FLAG";
    uint32_t * APP_FLAG = (uint32_t *)str_flag;

    /* Write received data in Flash */
    if (FLASH_If_Write(&flashdestination, (uint32_t*) APP_FLAG, 2)  == 0)
    {
      SerialPutString("\n\n\r APP Flag Set Successfully!\n\r--------------------------------\r\n");
      SerialPutString("\r\nBack to Main Menu, press 3 to run App...\r\n");
    }
    else /* An error occurred while writing to Flash memory */
    {
      /* End session */
      SerialPutString("\n\n\r APP Flag Set Error!\n\r--------------------------------\r\n");
    }

  }
  else if (Size == -1)
  {
    SerialPutString("\n\n\rThe image size is higher than the allowed space memory!\n\r");
  }
  else if (Size == -2)
  {
    SerialPutString("\n\n\rVerification failed!\n\r");
  }
  else if (Size == -3)
  {
    SerialPutString("\r\n\nAborted by user.\n\r");
  }
  else
  {
    SerialPutString("\n\rFailed to receive the file!\n\r");
  }
}

/**
  * @brief  Upload a file via serial port.
  * @param  None
  * @retval None
  */
void SerialUpload(void)
{
  uint8_t status = 0 ;

  SerialPutString("\n\n\rSelect Receive File\n\r");

  if (GetKey() == CRC16)
  {
    /* Transmit the flash image through ymodem protocol */
    status = Ymodem_Transmit((uint8_t*)APPLICATION_ADDRESS, (const uint8_t*)"UploadedFlashImage.bin", USER_FLASH_SIZE);

    if (status != 0)
    {
      SerialPutString("\n\rError Occurred while Transmitting File\n\r");
    }
    else
    {
      SerialPutString("\n\rFile uploaded successfully \n\r");
    }
  }
}

/**
  * @brief  Display the Main Menu on HyperTerminal
  * @param  None
  * @retval None
  */
void Main_Menu(void)
{
  uint8_t key = 0;

  /* 丢弃进入菜单前串口残留的旧字节（App 直跳/BLE 恢复期间可能混入） */
  Serial_Flush_RX();

  SerialPutString("\r\n======================================================================");
  SerialPutString("\r\n=              (C) COPYRIGHT 2026 STMicroelectronics                 =");
  SerialPutString("\r\n=                                                                    =");
  SerialPutString("\r\n=  STM32F4xx In-Application Programming Application  (Version 1.0.0) =");
  SerialPutString("\r\n=                                                                    =");
  SerialPutString("\r\n=                                   By MCD Application Team          =");
  SerialPutString("\r\n======================================================================");
  SerialPutString("\r\n\r\n");

  /* Test if any sector of Flash memory where user application will be loaded is write protected */
  if (FLASH_If_GetWriteProtectionStatus() == 0)
  {
    FlashProtection = 1;
  }
  else
  {
    FlashProtection = 0;
  }

  while (1)
  {
    SerialPutString("\r\n================== Main Menu ============================\r\n\n");
    SerialPutString("  Download Image To the STM32F4xx Internal Flash ------- 1\r\n\n");
    SerialPutString("  Upload Image From the STM32F4xx Internal Flash ------- 2\r\n\n");
    SerialPutString("  Execute The New Program ------------------------------ 3\r\n\n");

    if(FlashProtection != 0)
    {
      SerialPutString("  Disable the write protection ------------------------- 4\r\n\n");
    }

    SerialPutString("==========================================================\r\n\n");

    /* App 直跳进入时 BLE 链路保持在线，菜单打印一次即可 */
    key = GetKey();

    if (key == 0x31)
    {
      /* Download user application in the Flash */
      SerialDownload();
    }
    else if (key == 0x32)
    {
      /* Upload user application from the Flash */
      // SerialUpload();
      SerialPutString("This function is disabled! Please Use 1\r");
    }
    else if (key == 0x33) /* execute the new program */
    {
      Execute_New_Program();
    }
    else if ((key == 0x34) && (FlashProtection == 1))
    {
      /* Disable the write protection */
      switch (FLASH_If_DisableWriteProtection())
      {
        case 1:
        {
          SerialPutString("Write Protection disabled...\r\n");
          FlashProtection = 0;
          break;
        }
        case 2:
        {
          SerialPutString("Error: Flash write unprotection failed...\r\n");
          break;
        }
        default:
        {
        }
      }
    }
    else
    {
      if (FlashProtection == 0)
      {
        SerialPutString("Invalid Number ! ==> The number should be either 1, 2 or 3\r");
      }
      else
      {
        SerialPutString("Invalid Number ! ==> The number should be either 1, 2, 3 or 4\r");
      }
    }
  }
}

/**
  * @}
  */

/*******************(C)COPYRIGHT 2011 STMicroelectronics *****END OF FILE******/
