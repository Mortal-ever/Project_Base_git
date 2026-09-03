/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
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
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
     PC9   ------> RCC_MCO_2
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, PC13_LED_RUN_Pin|PC0_LED_ALM_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, PA4_LED_TF_Pin|PA15_SPI1_NSS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, PE8_DO_1_Pin|PE9_DO_2_Pin|PE10_DO_3_Pin|PE11_DO_4_Pin
                          |PE12_DO_5_Pin|PE13_DO_6_Pin|PE14_DO_7_Pin|PE15_DO_8_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : PE2_DI_3_Pin PE3_DI_4_Pin PE4_DI_5_Pin PE5_DI_6_Pin
                           PE6_DI_7_Pin PE7_DI_8_Pin PE0_DI_1_Pin PE1_DI_2_Pin */
  GPIO_InitStruct.Pin = PE2_DI_3_Pin|PE3_DI_4_Pin|PE4_DI_5_Pin|PE5_DI_6_Pin
                          |PE6_DI_7_Pin|PE7_DI_8_Pin|PE0_DI_1_Pin|PE1_DI_2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : PC13_LED_RUN_Pin */
  GPIO_InitStruct.Pin = PC13_LED_RUN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(PC13_LED_RUN_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PC0_LED_ALM_Pin */
  GPIO_InitStruct.Pin = PC0_LED_ALM_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(PC0_LED_ALM_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PC2 */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PA4_LED_TF_Pin PA15_SPI1_NSS_Pin */
  GPIO_InitStruct.Pin = PA4_LED_TF_Pin|PA15_SPI1_NSS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PB0_SDMMC0_DET_L_Pin */
  GPIO_InitStruct.Pin = PB0_SDMMC0_DET_L_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(PB0_SDMMC0_DET_L_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : ETH_RST_Pin */
  GPIO_InitStruct.Pin = ETH_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ETH_RST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PE8_DO_1_Pin PE9_DO_2_Pin PE10_DO_3_Pin PE11_DO_4_Pin
                           PE12_DO_5_Pin PE13_DO_6_Pin PE14_DO_7_Pin PE15_DO_8_Pin */
  GPIO_InitStruct.Pin = PE8_DO_1_Pin|PE9_DO_2_Pin|PE10_DO_3_Pin|PE11_DO_4_Pin
                          |PE12_DO_5_Pin|PE13_DO_6_Pin|PE14_DO_7_Pin|PE15_DO_8_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : PC9 */
  GPIO_InitStruct.Pin = GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF0_MCO;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : PD3_KEY1_Pin PD4_KEY2_Pin */
  GPIO_InitStruct.Pin = PD3_KEY1_Pin|PD4_KEY2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */
void dp83848_hw_reset(void)
{
	HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_RESET);
	HAL_Delay(20);
	HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_SET);
	HAL_Delay(20);
}
/* USER CODE END 2 */
