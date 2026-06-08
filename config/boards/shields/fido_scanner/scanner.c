#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(scanner_fido, LOG_LEVEL_DBG);

#define SCANNER_STACK_SIZE 1024
#define SCANNER_PRIORITY 7

K_THREAD_STACK_DEFINE(scanner_stack_area, SCANNER_STACK_SIZE);
struct k_thread scanner_thread_data;

void scanner_thread_func(void *arg1, void *arg2, void *arg3) {
    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

    uint8_t cmd_white_breathe[] = {
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 
        0x01, 0x00, 0x07,                   
        0x35, 0x01, 0x80, 0x07, 0x00,       
        0x00, 0xC5                          
    };

    while (1) {
        LOG_INF("========================================");
        LOG_INF("=  SCANNER FIDO THREAD IS ALIVE!!!     =");
        LOG_INF("========================================");

        if (!device_is_ready(uart_dev)) {
            LOG_ERR("UART1 device is NOT ready! Retrying in 5s...");
            k_msleep(5000);
            continue; 
        }

        LOG_INF("SUCCESS: UART1 is ready and active.");
        LOG_INF("Sending 16-byte wake-up command to GROW R502-F...");
        for (int i = 0; i < sizeof(cmd); i++) {
            uart_poll_out(uart_dev, cmd[i]);
        }
        
        k_msleep(5000); 
    }
}

static int init_scanner(void) {
    k_thread_create(&scanner_thread_data, scanner_stack_area,
                    K_THREAD_STACK_SIZEOF(scanner_stack_area),
                    scanner_thread_func,
                    NULL, NULL, NULL,
                    SCANNER_PRIORITY, 0, K_NO_WAIT);
    return 0;
}

SYS_INIT(init_scanner, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
