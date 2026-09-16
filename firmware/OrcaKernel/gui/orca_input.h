#ifndef ORCA_INPUT_H
#define ORCA_INPUT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Входящие события ввода из внешнего мира: тач с ПК (tools/fbstream,
 * --stream gui) и с Android-клиента через ESP32. Физического тачскрина
 * на плате ещё нет, поэтому пока это единственный источник координат.
 *
 * Продюсер — задача Orca Link (кадры ORCA_LINK_TOUCH/ORCA_LINK_BUTTON),
 * консьюмер — api->gui->poll_touch()/poll_button() в таске модуля.
 * Раз потоки разные, ринг защищён критической секцией: события короткие,
 * блокировать планировщик мьютексом ради двух uint16 смысла нет.
 *
 * Ринг намеренно маленький и перезаписывающий: свежий тап важнее старого,
 * а копить очередь, пока модуль не опрашивает ввод, бессмысленно.
 */

#define ORCA_INPUT_QUEUE_LEN   16u
#define ORCA_INPUT_BUTTONS     8u

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  display_id;
    uint8_t  action;      /* 0 = up, 1 = down, 2 = move (как в orca_link_touch_t) */
} orca_input_touch_t;

void orca_input_init(void);

/* Продюсер: вызывается из задачи Orca Link. */
void orca_input_push_touch(uint8_t display_id, uint16_t x, uint16_t y, uint8_t action);
void orca_input_set_button(uint8_t button_id, bool pressed);

/*
 * Консьюмер: снимает самое старое событие. false — очередь пуста.
 * display_id фильтруется вызывающим: событий с чужого дисплея не бывает,
 * пока дисплей один.
 */
bool orca_input_pop_touch(orca_input_touch_t* out);

/*
 * Сколько событий ещё лежит в ринге. Нужно драйверу ввода LVGL: его read_cb
 * обязан отдавать ровно одно состояние за вызов, иначе нажатие и отпускание,
 * пришедшие пачкой, схлопнутся в одно последнее и тапа не случится. Пока
 * очередь не пуста, драйвер просит повторный вызов через
 * lv_indev_data_t.continue_reading — а для этого ему нужен именно остаток.
 */
uint32_t orca_input_pending(void);

/*
 * Последнее пришедшее касание, не снимая ничего с очереди. Нужно тем, кто
 * рисует состояние, а не обрабатывает события: тестовый генератор кадров
 * показывает метку под курсором, и по ней сразу видно, доходят ли тапы с
 * клиента вообще. false — с момента старта не было ни одного касания.
 */
bool orca_input_last_touch(orca_input_touch_t* out);

/* Текущее состояние удалённой кнопки (залипающее до следующего кадра). */
bool orca_input_button_state(uint8_t button_id);

/* Сколько событий потеряно из-за переполнения — видно в `link`. */
uint32_t orca_input_dropped(void);

#endif /* ORCA_INPUT_H */
