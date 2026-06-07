#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>

static int init_scanner(void) {
    /* 1. Находим наш порт, который мы открыли в файле .overlay */
    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));

    if (!device_is_ready(uart_dev)) {
        return -ENODEV; /* Если порт не проснулся, тихо выходим, чтобы не сломать клавиатуру */
    }

    /* 2. Ставим загрузку системы на паузу на полсекунды, 
       чтобы сам сканер успел включиться после получения питания 3.3V */
    k_msleep(500);

    /* 3. Магический пакет байтов для R502-F: "Включить синее кольцо, гореть постоянно" */
    uint8_t cmd[] = {
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, // Заголовок пакета и адрес устройства
        0x01, 0x00, 0x07,                   // Тип пакета (Команда) и длина оставшейся части
        0x35, 0x03, 0x00, 0x02, 0x00,       // Команда 0x35 (Aura LED), Режим 3 (Постоянно), Цвет 2 (Синий)
        0x00, 0x42                          // Контрольная сумма (чтобы сканер поверил нам)
    };

    /* 4. Отправляем байты по одному в пин TX (D6) */
    for (int i = 0; i < sizeof(cmd); i++) {
        uart_poll_out(uart_dev, cmd[i]);
    }

    return 0;
}

/* Регистрируем функцию в ядре Zephyr для запуска при старте (когда инициализируются приложения) */
SYS_INIT(init_scanner, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
