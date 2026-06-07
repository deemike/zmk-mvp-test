#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

/* Выделяем сканеру собственную память и создаем независимый поток */
#define SCANNER_STACK_SIZE 1024
#define SCANNER_PRIORITY 7

void scanner_thread_func(void *arg1, void *arg2, void *arg3) {
    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

    if (!device_is_ready(uart_dev)) {
        return;
    }

    /* Ждем 2 полных секунды! Даем ZMK спокойно запустить USB и Bluetooth, 
       а сканеру - полностью проснуться после подачи питания */
    k_msleep(2000);

    /* Магический пакет: Включить синее кольцо */
    uint8_t cmd[] = {
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 
        0x01, 0x00, 0x07,                   
        0x35, 0x03, 0x00, 0x02, 0x00,       
        0x00, 0x42                          
    };

    for (int i = 0; i < sizeof(cmd); i++) {
        uart_poll_out(uart_dev, cmd[i]);
    }
}

/* Ядро Zephyr само запустит этот поток параллельно клавиатуре */
K_THREAD_DEFINE(scanner_tid, SCANNER_STACK_SIZE,
                scanner_thread_func, NULL, NULL, NULL,
                SCANNER_PRIORITY, 0, 0);
