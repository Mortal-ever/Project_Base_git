/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define PE2_DI_3_Pin GPIO_PIN_2
#define PE2_DI_3_GPIO_Port GPIOE
#define PE3_DI_4_Pin GPIO_PIN_3
#define PE3_DI_4_GPIO_Port GPIOE
#define PE4_DI_5_Pin GPIO_PIN_4
#define PE4_DI_5_GPIO_Port GPIOE
#define PE5_DI_6_Pin GPIO_PIN_5
#define PE5_DI_6_GPIO_Port GPIOE
#define PE6_DI_7_Pin GPIO_PIN_6
#define PE6_DI_7_GPIO_Port GPIOE
#define PC13_LED_RUN_Pin GPIO_PIN_13
#define PC13_LED_RUN_GPIO_Port GPIOC
#define PC0_LED_ALM_Pin GPIO_PIN_0
#define PC0_LED_ALM_GPIO_Port GPIOC
#define PA4_LED_TF_Pin GPIO_PIN_4
#define PA4_LED_TF_GPIO_Port GPIOA
#define PB0_SDMMC0_DET_L_Pin GPIO_PIN_0
#define PB0_SDMMC0_DET_L_GPIO_Port GPIOB
#define ETH_RST_Pin GPIO_PIN_1
#define ETH_RST_GPIO_Port GPIOB
#define PE7_DI_8_Pin GPIO_PIN_7
#define PE7_DI_8_GPIO_Port GPIOE
#define PE8_DO_1_Pin GPIO_PIN_8
#define PE8_DO_1_GPIO_Port GPIOE
#define PE9_DO_2_Pin GPIO_PIN_9
#define PE9_DO_2_GPIO_Port GPIOE
#define PE10_DO_3_Pin GPIO_PIN_10
#define PE10_DO_3_GPIO_Port GPIOE
#define PE11_DO_4_Pin GPIO_PIN_11
#define PE11_DO_4_GPIO_Port GPIOE
#define PE12_DO_5_Pin GPIO_PIN_12
#define PE12_DO_5_GPIO_Port GPIOE
#define PE13_DO_6_Pin GPIO_PIN_13
#define PE13_DO_6_GPIO_Port GPIOE
#define PE14_DO_7_Pin GPIO_PIN_14
#define PE14_DO_7_GPIO_Port GPIOE
#define PE15_DO_8_Pin GPIO_PIN_15
#define PE15_DO_8_GPIO_Port GPIOE
#define PA15_SPI1_NSS_Pin GPIO_PIN_15
#define PA15_SPI1_NSS_GPIO_Port GPIOA
#define PD3_KEY1_Pin GPIO_PIN_3
#define PD3_KEY1_GPIO_Port GPIOD
#define PD4_KEY2_Pin GPIO_PIN_4
#define PD4_KEY2_GPIO_Port GPIOD
#define PE0_DI_1_Pin GPIO_PIN_0
#define PE0_DI_1_GPIO_Port GPIOE
#define PE1_DI_2_Pin GPIO_PIN_1
#define PE1_DI_2_GPIO_Port GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
