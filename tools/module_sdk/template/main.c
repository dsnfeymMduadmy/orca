#include "orca_api.h"

int orca_app_main(const orca_api_t* api)
{
    api->log->info("template", "module started");

    while (!api->task->should_stop()) {
        api->task->sleep_ms(500);
    }

    api->log->info("template", "module stopped");
    return 0;
}

void orca_app_stop(void)
{
    /* освобождение ресурсов, если модуль что-то захватывал (gpio/uart/spi) */
}
