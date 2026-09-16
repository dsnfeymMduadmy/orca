#include "orca_input.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static orca_input_touch_t s_queue[ORCA_INPUT_QUEUE_LEN];
static uint32_t s_head;      /* индекс записи   */
static uint32_t s_tail;      /* индекс чтения   */
static uint32_t s_count;
static uint32_t s_dropped;
static uint8_t  s_buttons;   /* битовая маска нажатых */
static orca_input_touch_t s_last;
static bool     s_last_valid;

void orca_input_init(void)
{
    taskENTER_CRITICAL();
    memset(s_queue, 0, sizeof(s_queue));
    s_head = s_tail = s_count = s_dropped = 0;
    s_buttons = 0;
    memset(&s_last, 0, sizeof(s_last));
    s_last_valid = false;
    taskEXIT_CRITICAL();
}

void orca_input_push_touch(uint8_t display_id, uint16_t x, uint16_t y, uint8_t action)
{
    taskENTER_CRITICAL();

    if (s_count == ORCA_INPUT_QUEUE_LEN) {
        /*
         * Переполнение: выбрасываем самое старое, а не новое. Модуль,
         * который на секунду задумался, должен увидеть текущее положение
         * пальца, а не тап, случившийся секунду назад.
         */
        s_tail = (s_tail + 1u) % ORCA_INPUT_QUEUE_LEN;
        s_count--;
        s_dropped++;
    }

    s_queue[s_head].x          = x;
    s_queue[s_head].y          = y;
    s_queue[s_head].display_id = display_id;
    s_queue[s_head].action     = action;
    s_last       = s_queue[s_head];
    s_last_valid = true;
    s_head = (s_head + 1u) % ORCA_INPUT_QUEUE_LEN;
    s_count++;

    taskEXIT_CRITICAL();
}

bool orca_input_last_touch(orca_input_touch_t* out)
{
    if (out == NULL) {
        return false;
    }
    bool valid;
    taskENTER_CRITICAL();
    valid = s_last_valid;
    if (valid) {
        *out = s_last;
    }
    taskEXIT_CRITICAL();
    return valid;
}

bool orca_input_pop_touch(orca_input_touch_t* out)
{
    if (out == NULL) {
        return false;
    }

    bool got = false;
    taskENTER_CRITICAL();
    if (s_count > 0) {
        *out = s_queue[s_tail];
        s_tail = (s_tail + 1u) % ORCA_INPUT_QUEUE_LEN;
        s_count--;
        got = true;
    }
    taskEXIT_CRITICAL();
    return got;
}

uint32_t orca_input_pending(void)
{
    uint32_t n;
    taskENTER_CRITICAL();
    n = s_count;
    taskEXIT_CRITICAL();
    return n;
}

void orca_input_set_button(uint8_t button_id, bool pressed)
{
    if (button_id >= ORCA_INPUT_BUTTONS) {
        return;
    }
    taskENTER_CRITICAL();
    if (pressed) {
        s_buttons |= (uint8_t)(1u << button_id);
    } else {
        s_buttons &= (uint8_t)~(1u << button_id);
    }
    taskEXIT_CRITICAL();
}

bool orca_input_button_state(uint8_t button_id)
{
    if (button_id >= ORCA_INPUT_BUTTONS) {
        return false;
    }
    /*
     * Маску пишет задача линка (кадры BUTTON), читает задача модуля.
     * Все остальные обращения к общему состоянию в этом файле защищены
     * критической секцией; голое чтение здесь было исключением, о котором
     * пришлось бы помнить при каждой правке структуры состояния.
     */
    uint8_t mask;
    taskENTER_CRITICAL();
    mask = s_buttons;
    taskEXIT_CRITICAL();
    return (mask & (1u << button_id)) != 0;
}

uint32_t orca_input_dropped(void)
{
    uint32_t n;
    taskENTER_CRITICAL();
    n = s_dropped;
    taskEXIT_CRITICAL();
    return n;
}
