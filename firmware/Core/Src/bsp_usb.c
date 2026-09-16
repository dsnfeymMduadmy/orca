#include "main.h"
#include "usbd_core.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>

/*
 * Низкий уровень USB Device: HAL PCD на USB2_OTG_FS плюс мост
 * USBD_LL_* -> HAL_PCD_*, которого требует апстримный usbd_core.
 *
 * Класса здесь нет вообще — Mass Storage живёт в
 * OrcaKernel/link/orca_usb_msc.c. Этот файл про пины, тактирование,
 * распределение FIFO и прерывание.
 *
 * Пины (других на плате нет):
 *   DM  PA11 (AF10, OTG2_FS)
 *   DP  PA12 (AF10, OTG2_FS)
 *
 * VBUS на MCU не заведён, поэтому vbus_sensing выключен: с включенным
 * ядро USB считает, что шины нет, и молча не выходит из reset.
 *
 * Тактирование трансивера — HSI48 (выбран в board.c). Сам по себе HSI48
 * для full-speed на границе допуска (нужно ±0.25%), поэтому включаем CRS:
 * он подстраивает генератор по SOF хоста, то есть частота приходит от
 * хоста, а не от нашей калибровки. Кварц под USB не занимаем: PLL3
 * зарезервирована под пиксельклок LTDC.
 */

#define USB_IRQ_PRIORITY    6u   /* ниже configMAX_SYSCALL (5) — как у UART */

/*
 * Приоритет задачи, которая разбирает прерывание (см. OTG_FS_IRQHandler).
 * Выше всех прикладных задач: хост ждёт ответа на команду единицы
 * миллисекунд, и уступать шину отрисовке GUI здесь нельзя.
 */
#define USB_TASK_PRIORITY   (tskIDLE_PRIORITY + 4)

/*
 * 1024 слова (4 КБ). В этой задаче исполняется весь стек USB вместе с
 * колбэками хранилища: SCSI READ(10) уходит в HAL_SD_ReadBlocks, а тот
 * держит на стеке дескрипторы команды SDMMC.
 */
#define USB_TASK_STACK      1024u

/*
 * FIFO USB2_OTG_FS — 1.25 КБ (320 слов) на всё: приём и по одному
 * передающему FIFO на endpoint. 320 слов расходятся ровно: 128 на RX,
 * 64 на EP0 и 128 на EP1 (bulk-in MSC). Больше EP1 не выделить, а меньше
 * — заметно просядет скорость чтения.
 */
#define USB_RX_FIFO_WORDS   0x80u
#define USB_TX0_FIFO_WORDS  0x40u
#define USB_TX1_FIFO_WORDS  0x80u

static PCD_HandleTypeDef s_hpcd;
static TaskHandle_t      s_irq_task;

static USBD_StatusTypeDef usb_status(HAL_StatusTypeDef hal)
{
    switch (hal) {
    case HAL_OK:    return USBD_OK;
    case HAL_BUSY:  return USBD_BUSY;
    default:        return USBD_FAIL;
    }
}

/* Подстройка HSI48 по SOF хоста; до подключения просто ждёт синхросигнал. */
static void usb_crs_init(void)
{
    RCC_CRSInitTypeDef crs = {0};

    __HAL_RCC_CRS_CLK_ENABLE();

    crs.Prescaler             = RCC_CRS_SYNC_DIV1;
    crs.Source                = RCC_CRS_SYNC_SOURCE_USB2;
    crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue           = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000u, 1000u);
    crs.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;

    HAL_RCCEx_CRSConfig(&crs);
}

void HAL_PCD_MspInit(PCD_HandleTypeDef* pcd)
{
    if (pcd->Instance != USB2_OTG_FS) {
        return;
    }

    /*
     * Трансивер питается от VDD33USB. Пока детектор питания выключен,
     * ядро не видит на трансивере 3.3 В и держит линии в Z — хост не
     * замечает устройства вообще.
     */
    HAL_PWREx_EnableUSBVoltageDetector();

    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_11 | GPIO_PIN_12;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF10_OTG2_FS;
    HAL_GPIO_Init(GPIOA, &gpio);

    __HAL_RCC_USB2_OTG_FS_CLK_ENABLE();
    /* ULPI-такт нужен только внешнему HS-PHY; у нас встроенный FS. */
    __HAL_RCC_USB2_OTG_FS_ULPI_CLK_DISABLE();

    usb_crs_init();

    HAL_NVIC_SetPriority(OTG_FS_IRQn, USB_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef* pcd)
{
    if (pcd->Instance != USB2_OTG_FS) {
        return;
    }

    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
    __HAL_RCC_USB2_OTG_FS_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_11 | GPIO_PIN_12);
}

/*
 * Прерывание только будит задачу.
 *
 * Так сделано не ради «красоты», а потому что иначе устройство не работает.
 * Стек ST разбирает BOT прямо в обработчике: HAL_PCD_IRQHandler ->
 * USBD_LL_DataOutStage -> ... -> колбэк st_read, а тот уходит в
 * HAL_SD_ReadBlocks. Драйверы SD и QSPI отмеряют таймауты по HAL_GetTick(),
 * то есть по SysTick — а он висит на приоритете 15, ниже нашего 6. Внутри
 * этого обработчика тик не идёт, любое ожидание готовности карты никогда не
 * истекает, и первое же чтение вешает USB намертво: хост видит
 * DID_NO_CONNECT, а потом теряет устройство с шины.
 *
 * Поэтому на время разбора глушим глобальное разрешение прерываний самого
 * ядра USB (GAHBCFG.GINTMSK): флаги в GINTSTS остаются висеть, задача
 * разберёт их и снимет маску. NVIC трогать нельзя — прерывание уровневое, и
 * без маскирования на стороне ядра USB мы бы вернулись из обработчика в него
 * же, не дав задаче ни одного такта.
 */
void OTG_FS_IRQHandler(void)
{
    if (s_irq_task == NULL) {
        /* До создания задачи (её ещё нет при HAL_PCD_Init) — как обычно. */
        HAL_PCD_IRQHandler(&s_hpcd);
        return;
    }

    BaseType_t woken = pdFALSE;

    __HAL_PCD_DISABLE(&s_hpcd);
    vTaskNotifyGiveFromISR(s_irq_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void usb_irq_task(void* arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /*
         * Один вызов может не разгрести всё: пока мы читали сектор, хост
         * успел прислать следующую команду, а прерывание замаскировано и
         * будить нас больше некому. Крутимся, пока в GINTSTS есть флаги.
         */
        do {
            HAL_PCD_IRQHandler(&s_hpcd);
        } while ((USB2_OTG_FS->GINTSTS & USB2_OTG_FS->GINTMSK) != 0u);

        __HAL_PCD_ENABLE(&s_hpcd);
    }
}

/*
 * Задачу поднимаем до USBD_Start: с момента, когда ядро USB начнёт отвечать
 * хосту, обработчик уже должен уметь передавать работу в задачу.
 */
bool bsp_usb_irq_start(void)
{
    if (s_irq_task != NULL) {
        return true;
    }
    return xTaskCreate(usb_irq_task, "usbd", USB_TASK_STACK, NULL,
                       USB_TASK_PRIORITY, &s_irq_task) == pdPASS;
}

/*
 * Обратные вызовы HAL -> ядро стека. Ничего своего здесь не делаем: любая
 * реакция на подключение/отключение — дело orca_usb_msc, который смотрит
 * на dev_state устройства из своей задачи.
 */
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef* pcd)
{
    USBD_LL_SetupStage((USBD_HandleTypeDef*)pcd->pData, (uint8_t*)pcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef* pcd, uint8_t epnum)
{
    USBD_LL_DataOutStage((USBD_HandleTypeDef*)pcd->pData, epnum,
                         pcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef* pcd, uint8_t epnum)
{
    USBD_LL_DataInStage((USBD_HandleTypeDef*)pcd->pData, epnum,
                        pcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef* pcd)
{
    USBD_LL_SOF((USBD_HandleTypeDef*)pcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef* pcd)
{
    USBD_HandleTypeDef* pdev = (USBD_HandleTypeDef*)pcd->pData;

    /* Ядро FS другой скорости и не умеет, но стек хочет услышать это явно. */
    USBD_LL_SetSpeed(pdev, USBD_SPEED_FULL);
    USBD_LL_Reset(pdev);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef* pcd)
{
    /*
     * Сюда же приходит физическое отключение кабеля: без датчика VBUS
     * «хост уснул» и «кабель выдернули» с точки зрения ядра USB — одно и
     * то же событие (шина простаивает дольше 3 мс). В сон MCU не уходим:
     * ядро продолжает работать без хоста.
     */
    USBD_LL_Suspend((USBD_HandleTypeDef*)pcd->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef* pcd)
{
    USBD_LL_Resume((USBD_HandleTypeDef*)pcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef* pcd, uint8_t epnum)
{
    USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef*)pcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef* pcd, uint8_t epnum)
{
    USBD_LL_IsoINIncomplete((USBD_HandleTypeDef*)pcd->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef* pcd)
{
    USBD_LL_DevConnected((USBD_HandleTypeDef*)pcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef* pcd)
{
    USBD_LL_DevDisconnected((USBD_HandleTypeDef*)pcd->pData);
}

/* Мост в обратную сторону: то, что стек требует от платы. */
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef* pdev)
{
    s_hpcd.pData = pdev;
    pdev->pData  = &s_hpcd;

    s_hpcd.Instance                     = USB2_OTG_FS;
    s_hpcd.Init.dev_endpoints           = 9;
    s_hpcd.Init.speed                   = PCD_SPEED_FULL;
    s_hpcd.Init.dma_enable              = DISABLE;   /* у FS-ядра DMA нет */
    s_hpcd.Init.ep0_mps                 = EP_MPS_64;
    s_hpcd.Init.phy_itface              = PCD_PHY_EMBEDDED;
    s_hpcd.Init.Sof_enable              = DISABLE;
    s_hpcd.Init.low_power_enable        = DISABLE;
    s_hpcd.Init.lpm_enable              = DISABLE;
    s_hpcd.Init.battery_charging_enable = DISABLE;
    s_hpcd.Init.vbus_sensing_enable     = DISABLE;
    s_hpcd.Init.use_dedicated_ep1       = DISABLE;
    s_hpcd.Init.use_external_vbus       = DISABLE;

    if (HAL_PCD_Init(&s_hpcd) != HAL_OK) {
        return USBD_FAIL;
    }

    HAL_PCDEx_SetRxFiFo(&s_hpcd, USB_RX_FIFO_WORDS);
    HAL_PCDEx_SetTxFiFo(&s_hpcd, 0, USB_TX0_FIFO_WORDS);
    HAL_PCDEx_SetTxFiFo(&s_hpcd, 1, USB_TX1_FIFO_WORDS);

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef* pdev)
{
    return usb_status(HAL_PCD_DeInit(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef* pdev)
{
    return usb_status(HAL_PCD_Start(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef* pdev)
{
    return usb_status(HAL_PCD_Stop(pdev->pData));
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef* pdev, uint8_t ep_addr,
                                  uint8_t ep_type, uint16_t ep_mps)
{
    return usb_status(HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type));
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef* pdev, uint8_t ep_addr)
{
    return usb_status(HAL_PCD_EP_Close(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef* pdev, uint8_t ep_addr)
{
    return usb_status(HAL_PCD_EP_Flush(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef* pdev, uint8_t ep_addr)
{
    return usb_status(HAL_PCD_EP_SetStall(pdev->pData, ep_addr));
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef* pdev, uint8_t ep_addr)
{
    return usb_status(HAL_PCD_EP_ClrStall(pdev->pData, ep_addr));
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef* pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef* pcd = (PCD_HandleTypeDef*)pdev->pData;

    if ((ep_addr & 0x80u) == 0x80u) {
        return pcd->IN_ep[ep_addr & 0x7Fu].is_stall;
    }
    return pcd->OUT_ep[ep_addr & 0x7Fu].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef* pdev, uint8_t dev_addr)
{
    return usb_status(HAL_PCD_SetAddress(pdev->pData, dev_addr));
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef* pdev, uint8_t ep_addr,
                                    uint8_t* pbuf, uint32_t size)
{
    return usb_status(HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size));
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef* pdev, uint8_t ep_addr,
                                          uint8_t* pbuf, uint32_t size)
{
    return usb_status(HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size));
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef* pdev, uint8_t ep_addr)
{
    return HAL_PCD_EP_GetRxCount(pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay)
{
    HAL_Delay(Delay);
}
