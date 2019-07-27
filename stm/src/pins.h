#ifndef PINS_H
#define PINS_H

#include "stm32f2xx_ll_gpio.h"
// Define a bunch of names depending on the active build target

#ifdef STM32F207xx

#define ESP_USART USART6
#define ESP_USART_Port GPIOG
#define ESP_USART_TX LL_GPIO_PIN_14
#define ESP_USART_RX LL_GPIO_PIN_9

#define SIGN_DATA_Port GPIOD
#define SIGN_GPIO_PERIPH LL_AHB1_GRP1_PERIPH_GPIOD

#define UART_DMA DMA2
#define UART_DMA_TX_Stream LL_DMA_STREAM_2
#define UART_DMA_RX_Stream LL_DMA_STREAM_1
#define UART_DMA_Channel LL_DMA_CHANNEL_5

#define UART_DMA_PERIPH LL_AHB1_GRP1_PERIPH_DMA2
#define UART_GPIO_PERIPH LL_AHB1_GRP1_PERIPH_GPIOG
#define UART_UART_PERIPH LL_APB2_GRP1_PERIPH_USART6
#define UART_ENABLE_CALL LL_APB2_GRP1_EnableClock

#endif

#ifdef STM32F205xx

#define ESP_USART USART2
#define ESP_USART_Port GPIOA
#define ESP_USART_TX LL_GPIO_PIN_2
#define ESP_USART_RX LL_GPIO_PIN_3

#define SIGN_DATA_Port GPIOC
#define SIGN_GPIO_PERIPH LL_AHB1_GRP1_PERIPH_GPIOC

#define UART_DMA DMA1
#define UART_DMA_TX_Stream LL_DMA_STREAM_6
#define UART_DMA_RX_Stream LL_DMA_STREAM_5
#define UART_DMA_Channel LL_DMA_CHANNEL_4

#define UART_DMA_PERIPH LL_AHB1_GRP1_PERIPH_DMA1
#define UART_GPIO_PERIPH LL_AHB1_GRP1_PERIPH_GPIOA
#define UART_UART_PERIPH LL_APB1_GRP1_PERIPH_USART2
#define UART_ENABLE_CALL LL_APB1_GRP1_EnableClock

#endif

#endif