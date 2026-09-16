#include "main.h"
#include "orca_panic.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Всё здесь работает на регистрах напрямую, без HAL и без FreeRTOS.
 * Причина: паника случается в контексте исключения с запрещёнными
 * прерываниями. HAL_UART_Transmit там бесполезен — он ждёт HAL_GetTick(),
 * а SysTick уже не тикает, так что таймаут никогда не истечёт. Мьютекс TX
 * в этот момент может быть занят задачей, которую мы больше не запустим.
 */

#define PANIC_TX_GUARD  2000000u

static void panic_putc(char c)
{
    /* Если USART1 не поднят, TXE не появится никогда — не залипаем. */
    if ((RCC->APB2ENR & RCC_APB2ENR_USART1EN) == 0u ||
        (USART1->CR1 & USART_CR1_UE) == 0u) {
        return;
    }
    uint32_t guard = PANIC_TX_GUARD;
    while ((USART1->ISR & USART_ISR_TXE_TXFNF) == 0u) {
        if (--guard == 0u) {
            return;
        }
    }
    USART1->TDR = (uint8_t)c;
}

static void panic_puts(const char* s)
{
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        panic_putc(*s++);
    }
}

static void panic_hex32(uint32_t v)
{
    panic_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t nib = (v >> i) & 0xFu;
        panic_putc((char)((nib < 10u) ? ('0' + nib) : ('A' + nib - 10u)));
    }
}

static void panic_field(const char* label, uint32_t v)
{
    panic_puts(label);
    panic_hex32(v);
    panic_puts("\r\n");
}

static const char* panic_name(uint32_t code)
{
    switch (code) {
        case ORCA_PANIC_NMI:        return "NMI";
        case ORCA_PANIC_HARDFAULT:  return "HardFault";
        case ORCA_PANIC_MEMMANAGE:  return "MemManage (MPU)";
        case ORCA_PANIC_BUSFAULT:   return "BusFault";
        case ORCA_PANIC_USAGEFAULT: return "UsageFault";
        case ORCA_PANIC_STACK_OVF:  return "Stack overflow";
        case ORCA_PANIC_MALLOC:     return "FreeRTOS malloc failed";
        case ORCA_PANIC_ASSERT:     return "configASSERT";
        case ORCA_PANIC_ERROR_HDLR: return "Error_Handler";
        default:                    return "unknown";
    }
}

/* Грубая задержка: точность не нужна, важно только различать вспышки. */
static void panic_delay_ms(uint32_t ms)
{
    while (ms-- > 0u) {
        for (volatile uint32_t i = 0; i < 24000u; i++) {
        }
    }
}

static void panic_led(uint16_t pin, bool on)
{
    /* Светодиоды на GPIOB, активный уровень низкий. */
    GPIOB->BSRR = on ? (uint32_t)pin << 16 : (uint32_t)pin;
}

void orca_panic(uint32_t code, const uint32_t* frame, const char* name)
{
    __disable_irq();

    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;

    panic_puts("\r\n\r\n*** ORCA PANIC: ");
    panic_puts(panic_name(code));
    if (name != NULL) {
        panic_puts(" [");
        panic_puts(name);
        panic_puts("]");
    }
    panic_puts(" ***\r\n");

    panic_field("  code  = ", code);
    panic_field("  CFSR  = ", cfsr);
    panic_field("  HFSR  = ", hfsr);
    if ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0u) {
        panic_field("  MMFAR = ", SCB->MMFAR);
    }
    if ((cfsr & SCB_CFSR_BFARVALID_Msk) != 0u) {
        panic_field("  BFAR  = ", SCB->BFAR);
    }

    if (frame != NULL) {
        panic_field("  PC    = ", frame[6]);
        panic_field("  LR    = ", frame[5]);
        panic_field("  xPSR  = ", frame[7]);
        panic_field("  R0    = ", frame[0]);
        panic_field("  R1    = ", frame[1]);
        panic_field("  R2    = ", frame[2]);
        panic_field("  R3    = ", frame[3]);
        panic_field("  R12   = ", frame[4]);
    }

    panic_puts("  LED0 мигает ");
    panic_putc((char)('0' + (code % 10u)));
    panic_puts(" раз — это тот же код.\r\n");

    /*
     * Дальше — только светодиод: он читается и без подключённого ПК.
     * Мигаем красным LED0 (LED1 на плате зелёный — им отказ не показывают),
     * зелёный гасим, чтобы код читался без примеси.
     */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    panic_led(LED1_Pin, false);

    for (;;) {
        for (uint32_t i = 0; i < code; i++) {
            panic_led(LED0_Pin, true);
            panic_delay_ms(150);
            panic_led(LED0_Pin, false);
            panic_delay_ms(200);
        }
        panic_delay_ms(1500);
    }
}
