#include "orca_ui.h"
#include "orca_lvgl.h"
#include "orca_input.h"
#include "orca_fbstream.h"
#include "orca_appmgr.h"
#include "orca_usb_msc.h"
#include "board.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * Формат страницы
 * ===============
 *
 * Текстовый файл, одна строка — один виджет, вложенность задаётся отступом
 * (таб считается за 4 пробела). Пустые строки и строки, начинающиеся с '#',
 * игнорируются.
 *
 *   <виджет> [текст] [ключ=значение]...
 *
 * Текст без ключа — подпись виджета; если в ней есть пробелы, её берут в
 * двойные кавычки. Порядок ключей не важен.
 *
 * Виджеты:
 *   page      — не рисуется, задаёт страницу целиком: title=, clock=yes,
 *               back=<страница>, bg=<цвет>. Если строки нет, шапки не будет.
 *   row/col   — контейнер, дети в строку/столбец
 *   card      — панель с заголовком, дети в столбец
 *   label     — надпись
 *   value     — строка «подпись слева, значение справа»
 *   switch    — строка с переключателем (bind=led0|led1)
 *   slider    — строка с ползунком и процентами (value=0..100)
 *   button    — кнопка: goto=<страница>, run=<путь.orca>, tap=yes
 *   applist   — список приложений из 0:/apps, тап запускает
 *   spacer    — растяжка, съедает свободное место
 *   footer    — нижняя полоса
 *
 * Ключи оформления: font=14|16|20|28|48, color=, bg=, w=, h=, grow=,
 * pad=, gap=. Размер — число пикселей, «50%» или «content». Цвет — имя из
 * палитры (bg/panel/line/fg/dim/accent/ok/warn/err) или #RRGGBB.
 *
 * Ключ bind= привязывает виджет к живому значению ядра (см. s_bind_name):
 * uptime, clock, date, heap, lvgl, frames, stream, touch, taps, apps, sd,
 * qspi, usb, page, version. Обновляются все привязки одним таймером,
 * раз в 500 мс.
 *
 * Подписи только латиницей: встроенные Montserrat в LVGL идут без кириллицы,
 * русский текст вышел бы пустыми прямоугольниками.
 */

#define UI_APPS_DIR     "0:/apps"
#define UI_APP_FILE     "app.orca"
#define UI_APP_MANIFEST "app.json"

/* Глубина вложенности: больше четырёх уровней на экране 480x320 не нужно. */
#define UI_DEPTH_MAX    8u
#define UI_TOKENS_MAX   12
#define UI_BINDS_MAX    24u
#define UI_ACTIONS_MAX  24u
#define UI_APPS_MAX     16u
#define UI_PATH_MAX     128u

/* Палитра — та же, что в окне клиента (tools/fbstream/orca_view.py). */
#define UI_COL_BG      0x16181dUL
#define UI_COL_PANEL   0x1e2128UL
#define UI_COL_LINE    0x333947UL
#define UI_COL_FG      0xe6e8eeUL
#define UI_COL_DIM     0x8b93a7UL
#define UI_COL_ACCENT  0x4c9affUL
#define UI_COL_OK      0x3ddc97UL
#define UI_COL_WARN    0xffb454UL
#define UI_COL_ERR     0xff5f56UL

typedef enum {
    UI_BIND_NONE = 0,
    UI_BIND_UPTIME,
    UI_BIND_CLOCK,
    UI_BIND_DATE,
    UI_BIND_HEAP,
    UI_BIND_LVGL,
    UI_BIND_FRAMES,
    UI_BIND_STREAM,
    UI_BIND_TOUCH,
    UI_BIND_TAPS,
    UI_BIND_APPS,
    UI_BIND_SD,
    UI_BIND_QSPI,
    UI_BIND_USB,
    UI_BIND_PAGE,
    UI_BIND_VERSION,
    UI_BIND_COUNT
} ui_bind_t;

static const char* const s_bind_name[UI_BIND_COUNT] = {
    "", "uptime", "clock", "date", "heap", "lvgl", "frames", "stream",
    "touch", "taps", "apps", "sd", "qspi", "usb", "page", "version"
};

typedef enum {
    UI_ACT_GOTO = 0,
    UI_ACT_RUN,
    UI_ACT_TAP
} ui_act_kind_t;

typedef struct {
    uint8_t kind;
    char    arg[64];
} ui_action_t;

typedef struct {
    lv_obj_t* obj;
    uint8_t   bind;
} ui_binding_t;

typedef struct {
    char name[ORCA_UI_NAME_MAX + 1];
    char path[UI_PATH_MAX];
} ui_app_t;

typedef struct {
    const char*      text;
    const lv_font_t* font;
    /* Цвета держим сырым RGB: lv_color_t в RGB565 теряет биты, а нам ещё
     * сравнивать «задано/не задано» и передавать значение дальше. */
    uint32_t         color;
    bool             has_color;
    uint32_t         bg;
    bool             has_bg;
    int32_t          w;
    int32_t          h;
    int32_t          grow;
    int32_t          pad;
    int32_t          gap;
    int32_t          value;
    ui_bind_t        bind;
    int8_t           led;
    const char*      go;
    const char*      run;
    const char*      back;
    bool             tap;
    bool             show_clock;
} ui_attr_t;

/*
 * Текст страницы держим в статическом буфере: разбор идёт по месту (токены —
 * указатели внутрь буфера), а класть 4 КБ на стек задачи GUI нельзя.
 */
static char s_text[ORCA_UI_FILE_MAX];

static ui_binding_t s_binds[UI_BINDS_MAX];
static uint32_t     s_binds_n;

/*
 * Действия кнопок скопированы из текста страницы, а не указывают в него:
 * s_text перезаписывается при разборе следующей страницы, а кнопки старого
 * экрана к этому моменту ещё живы (экран удаляется после lv_screen_load).
 */
static ui_action_t s_actions[UI_ACTIONS_MAX];
static uint32_t    s_actions_n;

static ui_app_t  s_apps[UI_APPS_MAX];
static uint32_t  s_apps_n;
static bool      s_apps_valid;

static char s_page[ORCA_UI_NAME_MAX + 1] = ORCA_UI_HOME;
static bool s_page_from_card;
static bool s_ready;

/*
 * Запрошенный переход. Ставит кто угодно (колбэк кнопки, shell), выполняет
 * только задача GUI из nav_timer_cb. Так решаются сразу две задачи: экран не
 * удаляется из обработчика события собственной кнопки, и в дерево виджетов
 * не лезет чужая задача — LVGL к этому не готов.
 */
static char           s_pending[ORCA_UI_NAME_MAX + 1];
static volatile bool  s_pending_set;

static uint32_t s_taps;

static bool page_build(const char* name);
static void apps_scan(void);

/* ------------------------------------------------------------------ */
/* Разбор значений                                                    */
/* ------------------------------------------------------------------ */

static bool name_is_safe(const char* name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    uint32_t len = 0;
    for (const char* p = name; *p != '\0'; p++, len++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return len <= ORCA_UI_NAME_MAX;
}

static const lv_font_t* font_by_size(int32_t px)
{
    switch (px) {
        case 16: return &lv_font_montserrat_16;
        case 20: return &lv_font_montserrat_20;
        case 28: return &lv_font_montserrat_28;
        case 48: return &lv_font_montserrat_48;
        default: return &lv_font_montserrat_14;
    }
}

static bool color_parse(const char* s, uint32_t* out)
{
    static const struct {
        const char* name;
        uint32_t    rgb;
    } tbl[] = {
        { "bg",     UI_COL_BG     },
        { "panel",  UI_COL_PANEL  },
        { "line",   UI_COL_LINE   },
        { "fg",     UI_COL_FG     },
        { "dim",    UI_COL_DIM    },
        { "accent", UI_COL_ACCENT },
        { "ok",     UI_COL_OK     },
        { "warn",   UI_COL_WARN   },
        { "err",    UI_COL_ERR    },
    };

    if (s == NULL || *s == '\0') {
        return false;
    }
    if (*s == '#') {
        *out = (uint32_t)strtoul(s + 1, NULL, 16);
        return true;
    }
    for (uint32_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (strcmp(tbl[i].name, s) == 0) {
            *out = tbl[i].rgb;
            return true;
        }
    }
    return false;
}

/* Размер: «120», «50%» или «content». */
static int32_t size_parse(const char* s)
{
    if (strcmp(s, "content") == 0) {
        return LV_SIZE_CONTENT;
    }
    int32_t n = (int32_t)strtol(s, NULL, 10);
    uint32_t len = (uint32_t)strlen(s);
    if (len > 0 && s[len - 1] == '%') {
        return lv_pct(n);
    }
    return n;
}

static bool is_yes(const char* s)
{
    return strcmp(s, "yes") == 0 || strcmp(s, "1") == 0 ||
           strcmp(s, "true") == 0 || strcmp(s, "on") == 0;
}

/*
 * Разбор строки на токены по месту. Кавычки нужны подписям с пробелами
 * ("kernel heap") и значениям ключей (title="Orca OS"), поэтому strtok не
 * годится: он не знает про кавычки.
 *
 * Пишем в ту же строку, но своим указателем: выброшенные кавычки сдвигают
 * текст влево, и писатель отстаёт от читателя. Разделитель проглатываем
 * до записи '\0' — иначе при совпадении указателей терминатор затирает
 * пробел, на который читатель ещё смотрит, и разбор обрывается.
 */
static int tokenize(char* s, char** tok, int max)
{
    int   n = 0;
    char* w = s;
    char* r = s;

    while (*r != '\0' && n < max) {
        while (*r == ' ' || *r == '\t') {
            r++;
        }
        if (*r == '\0') {
            break;
        }

        tok[n++] = w;
        bool quoted = false;
        while (*r != '\0' && (quoted || (*r != ' ' && *r != '\t'))) {
            if (*r == '"') {
                quoted = !quoted;
                r++;
                continue;
            }
            *w++ = *r++;
        }
        if (*r != '\0') {
            r++;
        }
        *w++ = '\0';
    }
    return n;
}

static void attr_parse(char** tok, int n, ui_attr_t* a)
{
    memset(a, 0, sizeof(*a));
    a->font  = &lv_font_montserrat_14;
    a->w     = INT32_MIN;
    a->h     = INT32_MIN;
    a->pad   = INT32_MIN;
    a->gap   = INT32_MIN;
    a->value = INT32_MIN;
    a->led   = -1;

    for (int i = 1; i < n; i++) {
        char* eq = strchr(tok[i], '=');
        if (eq == NULL) {
            if (a->text == NULL) {
                a->text = tok[i];
            }
            continue;
        }
        *eq = '\0';
        const char* k = tok[i];
        const char* v = eq + 1;

        if (strcmp(k, "text") == 0 || strcmp(k, "title") == 0) {
            a->text = v;
        } else if (strcmp(k, "font") == 0) {
            a->font = font_by_size((int32_t)strtol(v, NULL, 10));
        } else if (strcmp(k, "color") == 0) {
            a->has_color = color_parse(v, &a->color);
        } else if (strcmp(k, "bg") == 0) {
            a->has_bg = color_parse(v, &a->bg);
        } else if (strcmp(k, "w") == 0) {
            a->w = size_parse(v);
        } else if (strcmp(k, "h") == 0) {
            a->h = size_parse(v);
        } else if (strcmp(k, "grow") == 0) {
            a->grow = (int32_t)strtol(v, NULL, 10);
        } else if (strcmp(k, "pad") == 0) {
            a->pad = (int32_t)strtol(v, NULL, 10);
        } else if (strcmp(k, "gap") == 0) {
            a->gap = (int32_t)strtol(v, NULL, 10);
        } else if (strcmp(k, "value") == 0) {
            a->value = (int32_t)strtol(v, NULL, 10);
        } else if (strcmp(k, "goto") == 0) {
            a->go = v;
        } else if (strcmp(k, "run") == 0) {
            a->run = v;
        } else if (strcmp(k, "back") == 0) {
            a->back = v;
        } else if (strcmp(k, "tap") == 0) {
            a->tap = is_yes(v);
        } else if (strcmp(k, "clock") == 0) {
            a->show_clock = is_yes(v);
        } else if (strcmp(k, "bind") == 0) {
            if (strcmp(v, "led0") == 0) {
                a->led = 0;
            } else if (strcmp(v, "led1") == 0) {
                a->led = 1;
            } else {
                for (uint32_t b = 1; b < UI_BIND_COUNT; b++) {
                    if (strcmp(s_bind_name[b], v) == 0) {
                        a->bind = (ui_bind_t)b;
                        break;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Живые значения                                                     */
/* ------------------------------------------------------------------ */

static void bind_add(lv_obj_t* obj, ui_bind_t bind)
{
    if (obj == NULL || bind == UI_BIND_NONE || s_binds_n >= UI_BINDS_MAX) {
        return;
    }
    s_binds[s_binds_n].obj  = obj;
    s_binds[s_binds_n].bind = (uint8_t)bind;
    s_binds_n++;
}

static void bind_update(lv_obj_t* obj, ui_bind_t bind)
{
    switch (bind) {
        case UI_BIND_UPTIME: {
            uint32_t ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
            lv_label_set_text_fmt(obj, "%02u:%02u:%02u",
                                  (unsigned)(ms / 3600000u),
                                  (unsigned)((ms / 60000u) % 60u),
                                  (unsigned)((ms / 1000u) % 60u));
            break;
        }
        case UI_BIND_CLOCK:
        case UI_BIND_DATE: {
            if (!board_rtc_ready()) {
                lv_label_set_text(obj, "--");
                break;
            }
            orca_datetime_t dt;
            board_rtc_split((uint64_t)((int64_t)board_rtc_unix() +
                                       (int64_t)board_rtc_tz_offset() * 60), &dt);
            if (bind == UI_BIND_CLOCK) {
                lv_label_set_text_fmt(obj, "%02u:%02u:%02u",
                                      dt.hour, dt.minute, dt.second);
            } else {
                lv_label_set_text_fmt(obj, "%04u-%02u-%02u",
                                      dt.year, dt.month, dt.day);
            }
            break;
        }
        case UI_BIND_HEAP:
            lv_label_set_text_fmt(obj, "%u KB free",
                                  (unsigned)(xPortGetFreeHeapSize() / 1024u));
            break;
        case UI_BIND_LVGL: {
            lv_mem_monitor_t mon;
            lv_mem_monitor(&mon);
            lv_label_set_text_fmt(obj, "%u KB / %u%%",
                                  (unsigned)((mon.total_size - mon.free_size) / 1024u),
                                  (unsigned)mon.used_pct);
            break;
        }
        case UI_BIND_FRAMES:
            lv_label_set_text_fmt(obj, "%u",
                                  (unsigned)orca_lvgl_frame_count());
            break;
        case UI_BIND_STREAM: {
            orca_fbstream_stats_t st;
            orca_fbstream_get_stats(&st);
            if (st.enabled && st.source == (uint8_t)ORCA_FB_SRC_GUI) {
                lv_label_set_text_fmt(obj, "%u sent / %u lost",
                                      (unsigned)st.frames_sent,
                                      (unsigned)st.frames_dropped);
                lv_obj_set_style_text_color(obj, lv_color_hex(UI_COL_OK), LV_PART_MAIN);
            } else {
                lv_label_set_text(obj, st.enabled ? "fbtest" : "off");
                lv_obj_set_style_text_color(obj, lv_color_hex(UI_COL_DIM), LV_PART_MAIN);
            }
            break;
        }
        case UI_BIND_TOUCH: {
            orca_input_touch_t tp;
            if (orca_input_last_touch(&tp)) {
                lv_label_set_text_fmt(obj, "touch: %u, %u  %s",
                                      (unsigned)tp.x, (unsigned)tp.y,
                                      (tp.action == 0u) ? "up" : "down");
            } else {
                lv_label_set_text(obj, "touch: -");
            }
            break;
        }
        case UI_BIND_TAPS:
            lv_label_set_text_fmt(obj, "%u", (unsigned)s_taps);
            break;
        case UI_BIND_APPS:
            /* Страница может показывать счётчик, не показывая сам список. */
            if (!s_apps_valid) {
                apps_scan();
            }
            lv_label_set_text_fmt(obj, "%u", (unsigned)s_apps_n);
            break;
        case UI_BIND_SD:
            if (board_sdmmc_ready()) {
                lv_label_set_text(obj, s_page_from_card ? "card" : "card, no /gui");
            } else {
                lv_label_set_text(obj, "no card");
            }
            break;
        case UI_BIND_QSPI: {
            const char* err = board_qspi_error();
            if (err == NULL || err[0] == '\0') {
                lv_label_set_text(obj, "16 MB");
            } else {
                lv_label_set_text_fmt(obj, "fail: %s", err);
            }
            break;
        }
        case UI_BIND_USB:
            /*
             * Не «кабель вставлен/нет», а кто владеет носителями: host означает,
             * что тома отданы хосту и с карты сейчас не читается ничего —
             * именно это объясняет пустой список приложений на соседней странице.
             */
            lv_label_set_text(obj, orca_usb_msc_state_str());
            break;
        case UI_BIND_PAGE:
            lv_label_set_text(obj, s_page);
            break;
        case UI_BIND_VERSION:
            lv_label_set_text(obj, "LVGL " LVGL_VERSION_INFO);
            break;
        default:
            break;
    }
}

/*
 * Раз в 500 мс. Заодно это штатный источник изменений на экране: по бегущему
 * uptime видно, что задача GUI жива, даже когда никто ничего не трогает.
 */
static void refresh_timer_cb(lv_timer_t* t)
{
    (void)t;
    for (uint32_t i = 0; i < s_binds_n; i++) {
        bind_update(s_binds[i].obj, (ui_bind_t)s_binds[i].bind);
    }
}

/* ------------------------------------------------------------------ */
/* Переходы между страницами                                          */
/* ------------------------------------------------------------------ */

static void nav_request(const char* name)
{
    taskENTER_CRITICAL();
    strncpy(s_pending, name, ORCA_UI_NAME_MAX);
    s_pending[ORCA_UI_NAME_MAX] = '\0';
    s_pending_set = true;
    taskEXIT_CRITICAL();
}

static void nav_timer_cb(lv_timer_t* t)
{
    (void)t;
    if (!s_pending_set) {
        return;
    }

    char name[ORCA_UI_NAME_MAX + 1];
    taskENTER_CRITICAL();
    memcpy(name, s_pending, sizeof(name));
    s_pending_set = false;
    taskEXIT_CRITICAL();

    if (!page_build(name)) {
        printf("[WARN] gui: страница '%s' не собралась\r\n", name);
    }
}

/* ------------------------------------------------------------------ */
/* Действия                                                           */
/* ------------------------------------------------------------------ */

static void action_event_cb(lv_event_t* e)
{
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx == 0u || idx > s_actions_n) {
        return;
    }
    const ui_action_t* act = &s_actions[idx - 1u];

    switch (act->kind) {
        case UI_ACT_GOTO:
            nav_request(act->arg);
            break;
        case UI_ACT_RUN: {
            int32_t id = orca_appmgr_start(act->arg, 0);
            if (id < 0) {
                printf("[WARN] gui: не запустилось '%s'\r\n", act->arg);
            } else {
                printf("[OK] gui: запущено '%s', instance %ld\r\n",
                       act->arg, (long)id);
            }
            break;
        }
        case UI_ACT_TAP:
        default:
            s_taps++;
            break;
    }
}

/* Возвращает user_data для колбэка: 0 — действия нет. */
static void* action_add(ui_act_kind_t kind, const char* arg)
{
    if (s_actions_n >= UI_ACTIONS_MAX) {
        return (void*)0;
    }
    ui_action_t* act = &s_actions[s_actions_n];
    act->kind = (uint8_t)kind;
    if (arg != NULL) {
        strncpy(act->arg, arg, sizeof(act->arg) - 1);
        act->arg[sizeof(act->arg) - 1] = '\0';
    } else {
        act->arg[0] = '\0';
    }
    s_actions_n++;
    return (void*)(uintptr_t)s_actions_n;
}

static void led_event_cb(lv_event_t* e)
{
    lv_obj_t* sw  = lv_event_get_target_obj(e);
    uint8_t   led = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    board_led_set(led, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void slider_event_cb(lv_event_t* e)
{
    lv_obj_t* sl  = lv_event_get_target_obj(e);
    lv_obj_t* lbl = (lv_obj_t*)lv_event_get_user_data(e);
    if (lbl != NULL) {
        lv_label_set_text_fmt(lbl, "%d%%", (int)lv_slider_get_value(sl));
    }
}

/* ------------------------------------------------------------------ */
/* Кирпичи                                                            */
/* ------------------------------------------------------------------ */

static void style_flat(lv_obj_t* obj)
{
    /* Заготовка LVGL — белая карточка с рамкой; нам нужен прозрачный контейнер. */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(obj, 0, LV_PART_MAIN);
}

static lv_obj_t* flat_box(lv_obj_t* parent)
{
    lv_obj_t* box = lv_obj_create(parent);
    style_flat(box);
    return box;
}

static lv_obj_t* mk_label(lv_obj_t* parent, const char* text,
                          const lv_font_t* font, uint32_t rgb)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, (text != NULL) ? text : "");
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), LV_PART_MAIN);
    return l;
}

/* Строка: подпись слева, значение (или виджет) справа. */
static lv_obj_t* mk_row(lv_obj_t* parent, const char* name, const lv_font_t* font)
{
    lv_obj_t* row = flat_box(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (name != NULL && name[0] != '\0') {
        mk_label(row, name, font, UI_COL_DIM);
    }
    return row;
}

static lv_obj_t* mk_button(lv_obj_t* parent, const char* text,
                           const lv_font_t* font, void* user_data)
{
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN);
    lv_obj_center(mk_label(btn, text, font, UI_COL_BG));
    if (user_data != (void*)0) {
        lv_obj_add_event_cb(btn, action_event_cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

/* ------------------------------------------------------------------ */
/* Список приложений                                                  */
/* ------------------------------------------------------------------ */

/*
 * Имя из app.json, если оно там есть. Полноценный разбор JSON ради одного
 * поля не нужен: ищем "name" и берём следующую строку в кавычках. Нет файла
 * или поля — покажем имя каталога, это тоже осмысленно.
 */
static void app_pretty_name(const char* dir, const char* fallback,
                            char* out, uint32_t out_sz)
{
    strncpy(out, fallback, out_sz - 1);
    out[out_sz - 1] = '\0';

    char path[UI_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, UI_APP_MANIFEST) <= 0) {
        return;
    }

    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        return;
    }

    char  buf[192];
    UINT  read = 0;
    FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &read);
    f_close(&f);
    if (fr != FR_OK || read == 0) {
        return;
    }
    buf[read] = '\0';

    const char* p = strstr(buf, "\"name\"");
    if (p == NULL) {
        return;
    }
    p = strchr(p + 6, ':');
    if (p == NULL) {
        return;
    }
    p = strchr(p, '"');
    if (p == NULL) {
        return;
    }
    p++;
    const char* end = strchr(p, '"');
    if (end == NULL || end == p) {
        return;
    }

    uint32_t len = (uint32_t)(end - p);
    if (len > out_sz - 1) {
        len = out_sz - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
}

static void apps_scan(void)
{
    s_apps_n     = 0;
    s_apps_valid = true;

    DIR dir;
    if (f_opendir(&dir, UI_APPS_DIR) != FR_OK) {
        return;
    }

    FILINFO fno;
    while (s_apps_n < UI_APPS_MAX) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') {
            break;
        }

        ui_app_t* app = &s_apps[s_apps_n];
        if (fno.fattrib & AM_DIR) {
            /* Штатная раскладка: /apps/<имя>/app.orca + app.json. */
            char sub[UI_PATH_MAX];
            if (snprintf(sub, sizeof(sub), "%s/%s", UI_APPS_DIR, fno.fname) <= 0) {
                continue;
            }
            if (snprintf(app->path, sizeof(app->path), "%s/%s", sub, UI_APP_FILE) <= 0) {
                continue;
            }
            if (f_stat(app->path, NULL) != FR_OK) {
                continue;
            }
            app_pretty_name(sub, fno.fname, app->name, sizeof(app->name));
        } else {
            /* Одиночный /apps/<имя>.orca — так удобнее кидать сборку на карту. */
            uint32_t len = (uint32_t)strlen(fno.fname);
            if (len <= 5u || strcmp(&fno.fname[len - 5], ".orca") != 0) {
                continue;
            }
            if (snprintf(app->path, sizeof(app->path), "%s/%s",
                         UI_APPS_DIR, fno.fname) <= 0) {
                continue;
            }
            uint32_t base = len - 5u;
            if (base > ORCA_UI_NAME_MAX) {
                base = ORCA_UI_NAME_MAX;
            }
            memcpy(app->name, fno.fname, base);
            app->name[base] = '\0';
        }
        s_apps_n++;
    }

    f_closedir(&dir);
}

static void applist_build(lv_obj_t* box, const ui_attr_t* a)
{
    if (!s_apps_valid) {
        apps_scan();
    }

    if (s_apps_n == 0) {
        lv_obj_t* l = mk_label(box, "no apps in /apps", a->font, UI_COL_DIM);
        lv_obj_set_width(l, lv_pct(100));
        return;
    }

    for (uint32_t i = 0; i < s_apps_n; i++) {
        lv_obj_t* btn = mk_button(box, s_apps[i].name, a->font,
                                  action_add(UI_ACT_RUN, s_apps[i].path));
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_PANEL), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_COL_LINE), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        /* Текст кнопки на панели, а не на акценте — перекрашиваем обратно. */
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0),
                                    lv_color_hex(UI_COL_FG), LV_PART_MAIN);
    }
}

/* ------------------------------------------------------------------ */
/* Виджеты                                                            */
/* ------------------------------------------------------------------ */

static void attr_apply(lv_obj_t* obj, const ui_attr_t* a)
{
    if (a->w != INT32_MIN) {
        lv_obj_set_width(obj, a->w);
    }
    if (a->h != INT32_MIN) {
        lv_obj_set_height(obj, a->h);
    }
    if (a->grow > 0) {
        lv_obj_set_flex_grow(obj, (uint8_t)a->grow);
    }
    if (a->pad != INT32_MIN) {
        lv_obj_set_style_pad_all(obj, a->pad, LV_PART_MAIN);
    }
    if (a->gap != INT32_MIN) {
        lv_obj_set_style_pad_row(obj, a->gap, LV_PART_MAIN);
        lv_obj_set_style_pad_column(obj, a->gap, LV_PART_MAIN);
    }
    if (a->has_bg) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(a->bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    }
}

static lv_obj_t* widget_create(lv_obj_t* parent, const char* type, const ui_attr_t* a)
{
    if (strcmp(type, "row") == 0 || strcmp(type, "col") == 0) {
        lv_obj_t* box = flat_box(parent);
        lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, (type[0] == 'r') ? LV_FLEX_FLOW_ROW
                                                   : LV_FLEX_FLOW_COLUMN);
        return box;
    }

    if (strcmp(type, "card") == 0) {
        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_PANEL), LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_LINE), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        if (a->text != NULL) {
            mk_label(card, a->text, a->font, UI_COL_DIM);
        }
        return card;
    }

    if (strcmp(type, "label") == 0) {
        lv_obj_t* l = mk_label(parent, (a->text != NULL) ? a->text : "-", a->font,
                               a->has_color ? a->color : UI_COL_FG);
        bind_add(l, a->bind);
        return l;
    }

    if (strcmp(type, "value") == 0) {
        lv_obj_t* row = mk_row(parent, a->text, a->font);
        lv_obj_t* val = mk_label(row, "-", a->font,
                                 a->has_color ? a->color : UI_COL_FG);
        bind_add(val, a->bind);
        return row;
    }

    if (strcmp(type, "switch") == 0) {
        lv_obj_t* row = mk_row(parent, a->text, a->font);
        lv_obj_t* sw  = lv_switch_create(row);
        lv_obj_set_size(sw, 40, 20);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COL_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_COL_ACCENT),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (a->led >= 0) {
            lv_obj_add_event_cb(sw, led_event_cb, LV_EVENT_VALUE_CHANGED,
                                (void*)(uintptr_t)a->led);
        }
        return row;
    }

    if (strcmp(type, "slider") == 0) {
        int32_t val = (a->value == INT32_MIN) ? 50 : a->value;

        lv_obj_t* box = flat_box(parent);
        lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box, 6, LV_PART_MAIN);

        lv_obj_t* row = mk_row(box, a->text, a->font);
        lv_obj_t* lbl = mk_label(row, "-", a->font, UI_COL_FG);
        lv_label_set_text_fmt(lbl, "%d%%", (int)val);

        lv_obj_t* sl = lv_slider_create(box);
        lv_obj_set_width(sl, lv_pct(100));
        lv_slider_set_value(sl, val, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(UI_COL_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(UI_COL_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(UI_COL_ACCENT), LV_PART_KNOB);
        lv_obj_add_event_cb(sl, slider_event_cb, LV_EVENT_VALUE_CHANGED, lbl);
        return box;
    }

    if (strcmp(type, "button") == 0) {
        void* ud = (void*)0;
        if (a->go != NULL) {
            ud = action_add(UI_ACT_GOTO, a->go);
        } else if (a->run != NULL) {
            ud = action_add(UI_ACT_RUN, a->run);
        } else if (a->tap) {
            ud = action_add(UI_ACT_TAP, NULL);
        }
        lv_obj_t* btn = mk_button(parent, (a->text != NULL) ? a->text : "?",
                                  a->font, ud);
        lv_obj_set_width(btn, lv_pct(100));
        if (a->has_bg) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(a->bg), LV_PART_MAIN);
        }
        return btn;
    }

    if (strcmp(type, "applist") == 0) {
        lv_obj_t* box = lv_obj_create(parent);
        lv_obj_set_width(box, lv_pct(100));
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(box, 8, LV_PART_MAIN);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        /* Приложений может быть больше, чем влезает: список скроллится. */
        lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
        applist_build(box, a);
        return box;
    }

    if (strcmp(type, "spacer") == 0) {
        lv_obj_t* box = flat_box(parent);
        lv_obj_set_size(box, 1, 1);
        lv_obj_set_flex_grow(box, (a->grow > 0) ? (uint8_t)a->grow : 1);
        return box;
    }

    if (strcmp(type, "footer") == 0) {
        lv_obj_t* box = flat_box(parent);
        lv_obj_set_size(box, lv_pct(100), 26);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(box, lv_color_hex(UI_COL_PANEL), LV_PART_MAIN);
        lv_obj_set_style_pad_hor(box, 12, LV_PART_MAIN);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        return box;
    }

    printf("[WARN] gui: неизвестный виджет '%s'\r\n", type);
    return NULL;
}

/* Шапка страницы: кнопка «назад», заголовок, часы. */
static void page_chrome(lv_obj_t* scr, const ui_attr_t* a)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(a->has_bg ? a->bg : UI_COL_BG),
                              LV_PART_MAIN);

    lv_obj_t* header = flat_box(scr);
    lv_obj_set_size(header, lv_pct(100), 44);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(UI_COL_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(UI_COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(header, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (a->back != NULL) {
        lv_obj_t* back = mk_button(header, LV_SYMBOL_LEFT, a->font,
                                   action_add(UI_ACT_GOTO, a->back));
        lv_obj_set_size(back, 40, 30);
    }

    mk_label(header, (a->text != NULL) ? a->text : "Orca",
             &lv_font_montserrat_20, UI_COL_FG);

    if (a->show_clock) {
        /* Часы прижимаем к правому краю: между ними и заголовком растяжка. */
        lv_obj_t* gap = flat_box(header);
        lv_obj_set_size(gap, 1, 1);
        lv_obj_set_flex_grow(gap, 1);
        bind_add(mk_label(header, "--:--:--", &lv_font_montserrat_16, UI_COL_DIM),
                 UI_BIND_CLOCK);
    }
}

/* ------------------------------------------------------------------ */
/* Разбор страницы                                                    */
/* ------------------------------------------------------------------ */

static void page_parse(lv_obj_t* scr, char* text)
{
    lv_obj_t* stack[UI_DEPTH_MAX];
    int32_t   indent[UI_DEPTH_MAX];
    uint32_t  depth = 0;

    stack[0]  = scr;
    indent[0] = -1;

    char* p = text;
    while (p != NULL && *p != '\0') {
        char* line = p;
        char* nl   = strpbrk(p, "\r\n");
        if (nl != NULL) {
            *nl = '\0';
            p   = nl + 1;
        } else {
            p = NULL;
        }

        int32_t ind = 0;
        while (*line == ' ' || *line == '\t') {
            ind += (*line == '\t') ? 4 : 1;
            line++;
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }

        char* tok[UI_TOKENS_MAX];
        int   n = tokenize(line, tok, UI_TOKENS_MAX);
        if (n == 0) {
            continue;
        }

        ui_attr_t a;
        attr_parse(tok, n, &a);

        if (strcmp(tok[0], "page") == 0) {
            page_chrome(scr, &a);
            continue;
        }

        /* Отступ меньше или равен — вышли из вложенных контейнеров. */
        while (depth > 0 && ind <= indent[depth]) {
            depth--;
        }

        lv_obj_t* obj = widget_create(stack[depth], tok[0], &a);
        if (obj == NULL) {
            continue;
        }
        attr_apply(obj, &a);

        if (depth + 1u < UI_DEPTH_MAX) {
            depth++;
            stack[depth]  = obj;
            indent[depth] = ind;
        }
    }
}

/*
 * Встроенные копии страниц. Без карты (или с пустым 0:/gui) устройство обязано
 * показать рабочий экран, а не чёрный кадр, — а заодно это единственный
 * образец формата, который невозможно потерять вместе с картой.
 */
static const char s_embedded_main[] =
    "page title=\"Orca OS\" clock=yes\n"
    "row grow=1 pad=10 gap=10\n"
    "  card SYSTEM grow=1\n"
    "    value uptime bind=uptime\n"
    "    value \"kernel heap\" bind=heap\n"
    "    value \"lvgl heap\" bind=lvgl\n"
    "    value \"gui frames\" bind=frames\n"
    "    value stream bind=stream\n"
    "    value usb bind=usb\n"
    "    spacer\n"
    "    button APPS goto=apps\n"
    "  card CONTROL w=172\n"
    "    switch LED0 bind=led0\n"
    "    switch LED1 bind=led1\n"
    "    slider \"drag me\" value=50\n"
    "    button TAP tap=yes\n"
    "    value taps bind=taps\n"
    "footer\n"
    "  label bind=touch color=dim\n"
    "  label bind=version color=line\n";

static const char s_embedded_apps[] =
    "page title=APPS clock=yes back=main\n"
    "col grow=1 pad=10 gap=8\n"
    "  applist grow=1\n"
    "footer\n"
    "  label bind=page color=dim\n"
    "  label bind=sd color=dim\n";

static const char* embedded_page(const char* name)
{
    if (strcmp(name, "main") == 0) {
        return s_embedded_main;
    }
    if (strcmp(name, "apps") == 0) {
        return s_embedded_apps;
    }
    return NULL;
}

/* Кладёт текст страницы в s_text. false — страницы нет ни на карте, ни внутри. */
static bool page_load(const char* name)
{
    char path[UI_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s%s",
                 ORCA_UI_DIR, name, ORCA_UI_EXT) > 0) {
        FIL f;
        if (f_open(&f, path, FA_READ) == FR_OK) {
            UINT read = 0;
            FRESULT fr = f_read(&f, s_text, sizeof(s_text) - 1, &read);
            f_close(&f);
            if (fr == FR_OK && read > 0) {
                s_text[read] = '\0';
                s_page_from_card = true;
                return true;
            }
        }
    }

    const char* embedded = embedded_page(name);
    if (embedded == NULL) {
        return false;
    }
    strncpy(s_text, embedded, sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = '\0';
    s_page_from_card = false;
    return true;
}

static bool page_build(const char* name)
{
    if (!name_is_safe(name) || !page_load(name)) {
        return false;
    }

    /*
     * Привязки и действия принадлежат экрану: он сейчас будет заменён, и
     * держать указатели на его виджеты нельзя ни мгновения дольше.
     */
    s_binds_n   = 0;
    s_actions_n = 0;

    lv_obj_t* scr = lv_obj_create(NULL);
    if (scr == NULL) {
        printf("[ERROR] gui: не хватило памяти LVGL на экран '%s'\r\n", name);
        return false;
    }
    style_flat(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COL_BG), LV_PART_MAIN);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    strncpy(s_page, name, sizeof(s_page) - 1);
    s_page[sizeof(s_page) - 1] = '\0';

    page_parse(scr, s_text);

    lv_obj_t* old = lv_screen_active();
    lv_screen_load(scr);
    if (old != NULL && old != scr) {
        lv_obj_delete(old);
    }

    refresh_timer_cb(NULL);
    return true;
}

/* ------------------------------------------------------------------ */
/* Публичный интерфейс                                                */
/* ------------------------------------------------------------------ */

bool orca_ui_init(void)
{
    s_binds_n   = 0;
    s_actions_n = 0;
    s_apps_valid = false;

    if (!page_build(ORCA_UI_HOME)) {
        printf("[ERROR] gui: домашняя страница не собралась\r\n");
        return false;
    }

    /*
     * Оба таймера создаются один раз и живут дольше любого экрана: они
     * работают с таблицей привязок, а не с конкретными виджетами.
     *
     * nav — 50 мс: переход выполняется не в колбэке нажавшей кнопки (удалять
     * экран, которому она принадлежит, посреди обработки её события нельзя),
     * а на следующем витке задачи GUI. Задержка на глаз незаметна, зато
     * orca_ui_open можно звать откуда угодно, включая shell.
     */
    lv_timer_create(nav_timer_cb, 50, NULL);
    lv_timer_create(refresh_timer_cb, 500, NULL);

    s_ready = true;
    printf("[OK] GUI: страница '%s' (%s), привязок %lu\r\n",
           s_page, s_page_from_card ? ORCA_UI_DIR : "встроенная",
           (unsigned long)s_binds_n);
    return true;
}

bool orca_ui_open(const char* name)
{
    if (!s_ready || !name_is_safe(name)) {
        return false;
    }

    /*
     * Проверяем наличие страницы сразу: вызывающий (shell) должен получить
     * ответ, а не обнаружить молчание, потому что сборка идёт асинхронно.
     */
    char path[UI_PATH_MAX];
    bool exists = (embedded_page(name) != NULL);
    if (!exists &&
        snprintf(path, sizeof(path), "%s/%s%s",
                 ORCA_UI_DIR, name, ORCA_UI_EXT) > 0) {
        exists = (f_stat(path, NULL) == FR_OK);
    }
    if (!exists) {
        return false;
    }

    nav_request(name);
    return true;
}

bool orca_ui_reload(void)
{
    if (!s_ready) {
        return false;
    }
    /* Каталог приложений мог измениться вместе со страницами. */
    s_apps_valid = false;
    nav_request(s_page);
    return true;
}

const char* orca_ui_current(void)
{
    return s_page;
}

bool orca_ui_from_card(void)
{
    return s_page_from_card;
}

void orca_ui_apps_changed(void)
{
    s_apps_valid = false;
}
