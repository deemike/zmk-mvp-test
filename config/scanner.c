#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>

static int init_scanner(void) {
    /* Обращаемся к uart1, который мы настроили в overlay */
    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

    if (!device_is_ready(uart_dev)) {
        return 0; /* Если порт не готов, просто выходим, чтобы не повесить систему */
    }

    /* Даем сканеру 500мс на загрузку после подачи питания */
    k_msleep(500);

    /* Команда для R502-F: "Включить синее кольцо, гореть постоянно" */
    uint8_t cmd[] = {
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 
        0x01, 0x00, 0x07,                   
        0x35, 0x03, 0x00, 0x02, 0x00,       
        0x00, 0x42                          
    };

    /* Отправляем байты по одному */
    for (int i = 0; i < sizeof(cmd); i++) {
        uart_poll_out(uart_dev, cmd[i]);
    }

    return 0;
}

SYS_INIT(init_scanner, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
