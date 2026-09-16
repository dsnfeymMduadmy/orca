#ifndef MAIN_H
#define MAIN_H

#include "stm32h7xx_hal.h"

void Error_Handler(void);

#define LED0_Pin GPIO_PIN_0
#define LED0_GPIO_Port GPIOB
#define LED1_Pin GPIO_PIN_1
#define LED1_GPIO_Port GPIOB

#define BTN_WK_UP_Pin GPIO_PIN_0
#define BTN_WK_UP_GPIO_Port GPIOA
#define BTN_KEY0_Pin GPIO_PIN_1
#define BTN_KEY0_GPIO_Port GPIOA

#endif
