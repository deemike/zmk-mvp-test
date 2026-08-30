#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(scanner_fido, LOG_LEVEL_DBG);

#define SCANNER_STACK_SIZE 1024
#define SCANNER_PRIORITY 7
#define RING_BUF_SIZE 128

K_THREAD_STACK_DEFINE(scanner_stack_area, SCANNER_STACK_SIZE);
struct k_thread scanner_thread_data;

/* Кольцевой буфер для асинхронного приема */
RING_BUF_DECLARE(rx_ringbuf, RING_BUF_SIZE);

/* Обработчик прерываний (ISR) для UART */
static void uart_cb(const struct device *dev, void *user_data) {
    uint8_t rx_data[16];
    int recv_len;

    if (!uart_irq_update(dev)) {
        return;
    }

    /* Если есть входящие данные на линии RX */
    if (uart_irq_rx_ready(dev)) {
        recv_len = uart_fifo_read(dev, rx_data, sizeof(rx_data));
        if (recv_len > 0) {
            /* Складываем принятые байты в кольцевой буфер */
            ring_buf_put(&rx_ringbuf, rx_data, recv_len);
        }
    }
}

void scanner_thread_func(void *arg1, void *arg2, void *arg3) {
    const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

    uint8_t cmd_white_breathe[] = {
        0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
        0x01, 0x00, 0x07,
        0x35, 0x01, 0xFF, 0x07, 0x00,
        0x01, 0x44
    };

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART1 device is NOT ready!");
        return;
    }

    LOG_INF("SUCCESS: UART1 is ready. Configuring interrupts...");
    
    /* Привязываем коллбэк и включаем прерывания на прием (RX) */
    uart_irq_callback_set(uart_dev, uart_cb);
    uart_irq_rx_enable(uart_dev);

    /* Тестовая отправка команды на сканер один раз при старте */
    for (int i = 0; i < sizeof(cmd_white_breathe); i++) {
        uart_poll_out(uart_dev, cmd_white_breathe[i]);
    }

    while (1) {
        uint8_t rx_byte;
        
        /* Читаем байты из буфера поштучно, пока они там есть */
        while (ring_buf_get(&rx_ringbuf, &rx_byte, 1) > 0) {
            LOG_INF("RX: %02X", rx_byte);
            /* В следующем шаге мы добавим сюда парсер для сборки пакета ответа */
        }

        /* Спим 100 мс, чтобы не грузить ядро, и снова проверяем буфер */
        k_msleep(100); 
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
