#include "main.h"
#include "orca_shell.h"
#include "orca_fbstream.h"
#include "orca_link_service.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stream_buffer.h"
#include <stdbool.h>
#include <string.h>

/*
 * USART1 -> CH340E -> USB Type-C. Системная консоль + транспорт Orca Link
 * для трансляции экрана на ПК (tools/fbstream).
 *
 * Пины: TX PA9, RX PA10 (AF7).
 *
 * Скорость по умолчанию 921600: на 115200 трансляция framebuffer
 * практически неюзабельна (см. расчёт в docs/hardware.md).
 * CH340E такую скорость держит.
 *
 * RX идёт через DMA в кольцевой буфер + IDLE-прерывание, чтобы не терять
 * байты при потоке от ПК; TX — блокирующий с мьютексом.
 */

#define UART_BAUDRATE     921600u
#define UART_RX_DMA_SIZE  512u
#define UART_RX_STREAM    2048u
#define UART_TX_CHUNK_MAX 4096u

UART_HandleTypeDef huart1;
static DMA_HandleTypeDef hdma_usart1_rx;

/* DMA-буфер должен лежать в некешируемой RAM_D2 (MPU регион 4). */
static uint8_t s_rx_dma[UART_RX_DMA_SIZE] __attribute__((section(".dma_buffer"), aligned(32)));
static StreamBufferHandle_t s_rx_stream;
static SemaphoreHandle_t    s_tx_mutex;
static uint32_t             s_rx_last_pos;

static void uart_drain_dma(void)
{
    uint32_t pos = UART_RX_DMA_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
    BaseType_t woken = pdFALSE;

    while (s_rx_last_pos != pos) {
        uint8_t b = s_rx_dma[s_rx_last_pos];
        xStreamBufferSendFromISR(s_rx_stream, &b, 1, &woken);
        s_rx_last_pos = (s_rx_last_pos + 1u) % UART_RX_DMA_SIZE;
    }
    portYIELD_FROM_ISR(woken);
}

void USART1_IRQHandler(void)
{
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE)) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart1);
        uart_drain_dma();
    }
    HAL_UART_IRQHandler(&huart1);
}

void DMA1_Stream0_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart1_rx);
    uart_drain_dma();
}

static int uart_read(uint8_t* buf, uint32_t len, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY
                                                      : pdMS_TO_TICKS(timeout_ms);
    size_t n = xStreamBufferReceive(s_rx_stream, buf, len, ticks);
    return (int)n;
}

static int uart_write(const uint8_t* buf, uint32_t len)
{
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart1, (uint8_t*)buf, (uint16_t)len, 1000);
    xSemaphoreGive(s_tx_mutex);
    return (st == HAL_OK) ? (int)len : -1;
}

/*
 * Для fbstream: TX блокирующий и защищён мьютексом, поэтому "готовность"
 * определяется тем, свободен ли он. Если кто-то уже пишет — кадр дропается,
 * а не встаёт в очередь (иначе GUI начнёт залипать на трансляции).
 */
static uint32_t uart_writable(void)
{
    if (s_tx_mutex == NULL || uxSemaphoreGetCount(s_tx_mutex) == 0) {
        return 0;
    }
    return UART_TX_CHUNK_MAX;
}

/*
 * Вывод printf/newlib (retarget.c). Тот же мьютекс, что и у кадров Orca Link,
 * иначе текстовый лог рвёт бинарный кадр посередине. До инициализации порта
 * (мьютекса ещё нет) пишем напрямую — конкурировать всё равно не с кем.
 */
int bsp_uart_console_write(const uint8_t* buf, uint32_t len)
{
    if (s_tx_mutex == NULL) {
        return (HAL_UART_Transmit(&huart1, (uint8_t*)buf, (uint16_t)len, 1000) == HAL_OK)
                   ? (int)len : -1;
    }
    return uart_write(buf, len);
}

const orca_shell_io_t g_uart_shell_io = {
    .read  = uart_read,
    .write = uart_write
};

const orca_fbstream_transport_t g_uart_fbstream_transport = {
    .writable = uart_writable
};

/* Тот же порт для службы Orca Link — она владеет RX и демуксит поток. */
const orca_link_service_io_t g_uart_link_io = {
    .read  = uart_read,
    .write = uart_write
};

bool bsp_uart_init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart1.Instance                    = USART1;
    huart1.Init.BaudRate               = UART_BAUDRATE;
    huart1.Init.WordLength             = UART_WORDLENGTH_8B;
    huart1.Init.StopBits               = UART_STOPBITS_1;
    huart1.Init.Parity                 = UART_PARITY_NONE;
    huart1.Init.Mode                   = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        return false;
    }

    hdma_usart1_rx.Instance                 = DMA1_Stream0;
    hdma_usart1_rx.Init.Request             = DMA_REQUEST_USART1_RX;
    hdma_usart1_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_usart1_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart1_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart1_rx.Init.Mode                = DMA_CIRCULAR;
    hdma_usart1_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_usart1_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&hdma_usart1_rx) != HAL_OK) {
        return false;
    }
    __HAL_LINKDMA(&huart1, hdmarx, hdma_usart1_rx);

    s_rx_stream = xStreamBufferCreate(UART_RX_STREAM, 1);
    s_tx_mutex  = xSemaphoreCreateMutex();
    if (s_rx_stream == NULL || s_tx_mutex == NULL) {
        return false;
    }

    s_rx_last_pos = 0;
    if (HAL_UART_Receive_DMA(&huart1, s_rx_dma, UART_RX_DMA_SIZE) != HAL_OK) {
        return false;
    }
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);

    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);

    return true;
}

/*
 * Модемный UART (USART2, пины на разъёме P1) — порт, который отдаётся
 * загруженным модулям через orca_api.uart. Драйвера ещё нет (Milestone 2),
 * поэтому open() честно отвечает отказом: модуль увидит NULL-хендл и
 * сможет деградировать, а не упасть на вызове по мусорному указателю.
 */
bool bsp_modem_uart_open(uint32_t port_id, uint32_t baudrate)
{
    (void)port_id;
    (void)baudrate;
    return false;
}

int32_t bsp_modem_uart_read(uint32_t port_id, void* buf, uint32_t len, uint32_t timeout_ms)
{
    (void)port_id;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    return -1;
}

int32_t bsp_modem_uart_write(uint32_t port_id, const void* buf, uint32_t len)
{
    (void)port_id;
    (void)buf;
    (void)len;
    return -1;
}

void bsp_modem_uart_close(uint32_t port_id)
{
    (void)port_id;
}
