#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h> /* Библиотека логов */

/* Регистрируем наш модуль в системе логов */
LOG_MODULE_REGISTER(scanner_fido, LOG_LEVEL_DBG);

#define SCANNER_STACK_SIZE 1024
#define SCANNER_PRIORITY 7

void scanner_thread_func(void *arg1, void *arg2, void *arg3) {
    LOG_INF("--- SCANNER THREAD STARTED ---");

    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("CRITICAL ERROR: UART1 device is NOT ready!");
        return;
    }
    LOG_INF("SUCCESS: UART1 is ready and bound to pins.");

    uint8_t cmd[] = {
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 
        0x01, 0x00, 0x07,                   
        0x35, 0x03, 0x00, 0x02, 0x00,       
        0x00, 0x42                          
    };

    while (1) {
        LOG_INF("Sending 16-byte command to GROW R502-F...");
        for (int i = 0; i < sizeof(cmd); i++) {
            uart_poll_out(uart_dev, cmd[i]);
        }
        LOG_INF("Command sent. Sleeping for 2 seconds.");
        
        k_msleep(2000); 
    }
}

K_THREAD_DEFINE(scanner_tid, SCANNER_STACK_SIZE,
                scanner_thread_func, NULL, NULL, NULL,
                SCANNER_PRIORITY, 0, 0);
