/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
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
#include "adc.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "menu.h"
#include "delay.h"
#include "lcd.h"
#include "lcd_init.h"
#include "power.h"
#include "key.h"
#include "KT6328.h"
#include <string.h>
#include <stdio.h>
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

/* USER CODE BEGIN PV */

uint8_t boot_in_menu_flag = 0;

extern pFunction Jump_To_Application;
extern uint32_t JumpAddress;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ---- 极简 I2C bit-bang：关 EM7028 心率传感器 LED ----
 * 冷启动长按 KEY1 进 BootLoader 时，EM7028 的 HRS_CFG 可能还保留 APP 设的 0x08（LED 开），
 * BootLoader 不操作它就会一直亮。这里直接用 GPIO PB13(SDA)/PB14(SCL) 模拟 I2C，
 * 写 SOFT_RESET(0x0F) 寄存器复位 EM7028，LED 自然关闭。
 * 不引入完整 iic_hal.c，只做这一次写操作。
 */
#define EM7028_SDA_PIN  GPIO_PIN_13
#define EM7028_SCL_PIN  GPIO_PIN_14
#define EM7028_SDA_PORT GPIOB
#define EM7028_SCL_PORT GPIOB
#define EM7028_I2C_ADDR 0x24   /* 7-bit addr, 左移后 0x48 */
#define EM7028_REG_SOFT_RESET 0x0F

static void em7028_sda_out(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = EM7028_SDA_PIN;
    g.Mode = GPIO_MODE_OUTPUT_OD;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(EM7028_SDA_PORT, &g);
}
static void em7028_sda_in(void)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = EM7028_SDA_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(EM7028_SDA_PORT, &g);
}
static void em7028_sda(uint8_t v) { HAL_GPIO_WritePin(EM7028_SDA_PORT, EM7028_SDA_PIN, v ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static void em7028_scl(uint8_t v) { HAL_GPIO_WritePin(EM7028_SCL_PORT, EM7028_SCL_PIN, v ? GPIO_PIN_SET : GPIO_PIN_RESET); }
static uint8_t em7028_sda_read(void) { return HAL_GPIO_ReadPin(EM7028_SDA_PORT, EM7028_SDA_PIN) == GPIO_PIN_SET ? 1 : 0; }

static void em7028_i2c_start(void)
{
    em7028_sda_out();
    em7028_sda(1); delay_us(2);
    em7028_scl(1); delay_us(2);
    em7028_sda(0); delay_us(2);
    em7028_scl(0); delay_us(2);
}
static void em7028_i2c_stop(void)
{
    em7028_sda_out();
    em7028_scl(0); delay_us(2);
    em7028_sda(0); delay_us(2);
    em7028_scl(1); delay_us(2);
    em7028_sda(1); delay_us(2);
}
static void em7028_i2c_send_byte(uint8_t b)
{
    em7028_sda_out();
    for (int i = 0; i < 8; i++) {
        em7028_scl(0); delay_us(2);
        em7028_sda(b & 0x80); delay_us(2);
        b <<= 1;
        em7028_scl(1); delay_us(2);
    }
    em7028_scl(0); delay_us(2);
}
static uint8_t em7028_i2c_wait_ack(void)
{
    uint8_t retry = 10;
    em7028_sda_in();
    em7028_scl(1); delay_us(2);
    while (em7028_sda_read()) {
        if (--retry == 0) { em7028_sda_out(); em7028_i2c_stop(); return 1; }
        delay_us(2);
    }
    em7028_sda_out();
    em7028_scl(0); delay_us(2);
    return 0;
}

static void EM7028_Disable_LED(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* 先把 SCL 配成推挽输出 */
    {
        GPIO_InitTypeDef g = {0};
        g.Pin = EM7028_SCL_PIN;
        g.Mode = GPIO_MODE_OUTPUT_PP;
        g.Pull = GPIO_PULLUP;
        g.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(EM7028_SCL_PORT, &g);
    }
    /* 方案1：直接写 HRS_CFG=0x00 关 LED */
    em7028_i2c_start();
    em7028_i2c_send_byte(EM7028_I2C_ADDR << 1);
    if (em7028_i2c_wait_ack()) { em7028_i2c_stop(); return; }  /* 无应答，EM7028 可能没焊接/不在线 */
    em7028_i2c_send_byte(0x01);   /* HRS_CFG */
    em7028_i2c_wait_ack();
    em7028_i2c_send_byte(0x00);   /* 关 LED */
    em7028_i2c_wait_ack();
    em7028_i2c_stop();
    delay_us(10);
    /* 方案2保险：写 SOFT_RESET 寄存器复位 */
    em7028_i2c_start();
    em7028_i2c_send_byte(EM7028_I2C_ADDR << 1);
    if (em7028_i2c_wait_ack()) { em7028_i2c_stop(); return; }
    em7028_i2c_send_byte(EM7028_REG_SOFT_RESET);
    em7028_i2c_wait_ack();
    em7028_i2c_send_byte(0x00);
    em7028_i2c_wait_ack();
    em7028_i2c_stop();
}

static void JumpToApp(void)
{
  printf("Jump to app\r\n");
  SysTick->CTRL = 0X00;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;
  __disable_irq();

  JumpAddress = *(__IO uint32_t*) (APPLICATION_ADDRESS + 4);
  Jump_To_Application = (pFunction) JumpAddress;
  __set_MSP(*(__IO uint32_t*) APPLICATION_ADDRESS);
  Jump_To_Application();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
	SCB->VTOR = FLASH_BASE;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* Re-assert the power latch as early as possible so a software reset does
     not let the PMIC shut the board down before the rest of main runs. */
  Power_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* App 直跳进入时 PRIMASK=1，这里恢复全局中断使 HAL_Delay/SysTick 正常 */
  __enable_irq();

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  MX_TIM3_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */

	//sys delay
	delay_init();

  // 关闭 EM7028 心率传感器 LED（APP 跑过开过，冷启动进 BootLoader 后仍亮）
  EM7028_Disable_LED();

  //key
	Key_Port_Init();

	//BLE
	KT6328_GPIO_Init();
	KT6328_Enable();
	

	//PWM Start
	HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);

	//lcd
	LCD_Init();
	LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
	delay_ms(10);
	LCD_Set_Light(50);

  // === Software Upgrade Request: Check RTC Backup Register ===
  // App writes BKP1R=0xB007 via OV+UPGRADE then resets.
  // BootLoader detects this and enters upgrade mode.
  HAL_PWR_EnableBkUpAccess();
  if (RTC->BKP1R == 0xB007)
  {
      RTC->BKP1R = 0;  // Clear flag to prevent false trigger on next boot
      boot_in_menu_flag = 1;
      LCD_ShowString(72, LCD_H/2, (uint8_t*)"OTA Upgrade", WHITE, BLACK, 24, 0);
      delay_ms(2000);   // �ȴ� KT6328 BLE ģ块启动并重新连�ӣ�否�򴮿ڲ˵ノ޷�͸传
      FLASH_If_Init();
      Main_Menu();
  }

  //寮?机启动时如果按下KEY1, 进入boot中IAP升级模式
  if(HAL_GPIO_ReadPin(KEY1_PORT, KEY1_PIN) == 0)
  {
		// 延时判断是否真的按下
    delay_ms(500);
    if(HAL_GPIO_ReadPin(KEY1_PORT, KEY1_PIN) == 0)
    {
      
      LCD_ShowString(72, LCD_H/2, (uint8_t*)"Bootload", WHITE, BLACK, 24, 0);//12*6,16*8,24*12,32*16
      LCD_ShowString(32, LCD_H/2+48, (uint8_t*)"OV-Watch V2.4.1", WHITE, BLACK, 24, 0);

			boot_in_menu_flag = 1;
			
      //go in boot menu
      FLASH_If_Init();
      Main_Menu();
    }
  }

  //如果没按下KEY1, 且有APP程序, 则运行APP, 没有APP鍒?
  else
  {
    uint32_t data1, data2;
    uint32_t address = 0x08008000; // Flash 中数据的起始地址

    // 读取地址涓? 0x08008000 的数鎹?
    data1 = *(uint32_t *)address;

    // 读取地址涓? 0x08008004 的数鎹?
    data2 = *(uint32_t *)(address + sizeof(uint32_t));

    // 将数据转换为字符数组
    char str1[5], str2[5];
    memcpy(str1, &data1, sizeof(data1));
    memcpy(str2, &data2, sizeof(data2));
    str1[4] = '\0';
    str2[4] = '\0';

    // 拼接两个字符数组
    char combined_str[9];
    strcpy(combined_str, str1);
    strcat(combined_str, str2);

    // 妫?查是否与 "APP FLAG" 相同
    if (strcmp(combined_str, "APP FLAG") == 0)
    {
        // 如果相同则跳杞?
        printf("APP FLAG OK, jump to app\r\n");
        JumpToApp();
    }
		// APP FLAG ???? App ???????????? App ????
		else
		{
			uint32_t app_sp = *(__IO uint32_t *)APPLICATION_ADDRESS;
			uint32_t app_pc = *(__IO uint32_t *)(APPLICATION_ADDRESS + 4);
			if ((app_sp >= 0x20000000U) && (app_sp <= 0x2001FFFFU) &&
			    (app_pc >= 0x0800C000U) && (app_pc <= 0x0807FFFFU))
			{
				printf("No APP FLAG but valid app vector, jump to app\r\n");
				JumpToApp();
			}
			else
			{
				LCD_ShowString(74, LCD_H/2, (uint8_t*)"No App!", WHITE, BLACK, 24, 0);
				LCD_ShowString(32, LCD_H/2+48, (uint8_t*)"Please Download", WHITE, BLACK, 24, 0);
				HAL_Delay(1000);
			}
		}
  }


  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
		printf("run in boot while(1)\r\n");
    printf("there is no legal APP\r\n");
		HAL_Delay(500);
		Power_DisEnable();
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 100;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
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
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
