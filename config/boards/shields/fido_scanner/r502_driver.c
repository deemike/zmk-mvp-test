#include "r502_driver.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <hal/nrf_uarte.h>
#include <errno.h>

LOG_MODULE_DECLARE(scanner_fido, LOG_LEVEL_DBG);

RING_BUF_DECLARE(driver_rx_ringbuf, 256);
static struct k_mutex r502_lock;

void r502_driver_init(void) {
    k_mutex_init(&r502_lock);
    ring_buf_reset(&driver_rx_ringbuf);
}

void r502_driver_feed_rx(const uint8_t *data, size_t len) {
    if (data && len > 0) {
        ring_buf_put(&driver_rx_ringbuf, data, len);
    }
}

void r502_driver_notify_ack(const struct r502_ack_packet *packet) {
    ARG_UNUSED(packet);
}

/* Проверка здоровья аппаратного UARTE и восстановление при ошибках переполнения */
void r502_uart_health_check(const struct device *uart_dev) {
    if (!uart_dev) return;

    /* 1. Сброс битов ошибок в драйвере Zephyr */
    int err = uart_err_check(uart_dev);

    /* 2. Проверка аппаратных регистров Nordic UARTE1 */
    NRF_UARTE_Type *uarte = NRF_UARTE1;
    bool has_hw_error = nrf_uarte_event_check(uarte, NRF_UARTE_EVENT_ERROR);

    if (err != 0 || has_hw_error) {
        LOG_WRN("UARTE1 error detected (driver_err=0x%02X, hw_event=%d). Recovering receiver...",
                err, (int)has_hw_error);

        /* Принудительная остановка зависшего RX EasyDMA */
        nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STOPRX);
        k_busy_wait(15);

        /* Очистка всех аппаратных событий и флагов ошибок */
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_RXSTARTED);
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_RXTO);
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ENDRX);
        nrf_uarte_event_clear(uarte, NRF_UARTE_EVENT_ERROR);
        nrf_uarte_errorsrc_get_and_clear(uarte);

        /* Перезапуск приема EasyDMA на штатный 1-байтовый буфер Zephyr */
        uint8_t *rx_buf = nrf_uarte_rx_buffer_get(uarte);
        if (rx_buf != NULL) {
            nrf_uarte_rx_buffer_set(uarte, rx_buf, 1);
            nrf_uarte_task_trigger(uarte, NRF_UARTE_TASK_STARTRX);
        }
    }
}

int r502_send_command(const struct device *uart_dev,
                      uint8_t cmd,
                      const uint8_t *params,
                      size_t param_len,
                      struct r502_ack_packet *out_ack,
                      uint32_t timeout_ms) {
    uint8_t tx_buf[64];

    if (!uart_dev || !device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }

    int pkg_len = r502_build_command(tx_buf, sizeof(tx_buf), cmd, params, param_len);
    if (pkg_len < 0) {
        LOG_ERR("Failed to build command (buffer too small or invalid params)");
        return -EINVAL;
    }

    k_mutex_lock(&r502_lock, K_FOREVER);

    /* Проверяем здоровье UARTE перед отправкой: сбрасываем аппаратные ошибки */
    r502_uart_health_check(uart_dev);

    /* Если в кольцевом буфере скопился избыточный мусор, сбрасываем его */
    if (ring_buf_space_get(&driver_rx_ringbuf) < 64) {
        ring_buf_reset(&driver_rx_ringbuf);
    }

    /* Отправка данных по UART */
    for (int i = 0; i < pkg_len; i++) {
        uart_poll_out(uart_dev, tx_buf[i]);
    }

    /* Ожидание и сборка полного ACK пакета из кольцевого буфера */
    struct r502_parser local_parser;
    r502_parser_init(&local_parser);
    struct r502_ack_packet packet;
    memset(&packet, 0, sizeof(packet));

    int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;
    bool ack_received = false;

    while (k_uptime_get() < deadline) {
        uint8_t byte;
        if (ring_buf_get(&driver_rx_ringbuf, &byte, 1) > 0) {
            if (r502_parser_feed_byte(&local_parser, byte, &packet)) {
                if (packet.pid == R502_PID_ACK) {
                    LOG_INF("R502 ACK received: CMD=0x%02X, Code=0x%02X", cmd, packet.confirmation_code);
                    ack_received = true;
                    break;
                }
            }
        } else {
            k_msleep(2);
        }
    }

    /* Технологическая пауза 20 мс между командами для микроконтроллера сканера */
    k_msleep(20);

    if (!ack_received) {
        LOG_WRN("Timeout waiting for ACK on cmd 0x%02X", cmd);
        /* При таймауте проверяем состояние UARTE для предотвращения зависания RX */
        r502_uart_health_check(uart_dev);
        k_mutex_unlock(&r502_lock);
        return -ETIMEDOUT;
    }

    if (!packet.valid) {
        LOG_ERR("Received invalid ACK packet (bad checksum)");
        k_mutex_unlock(&r502_lock);
        return -EIO;
    }

    if (out_ack) {
        *out_ack = packet;
    }

    int code = packet.confirmation_code;
    k_mutex_unlock(&r502_lock);

    return code;
}

int r502_set_led(const struct device *uart_dev,
                 uint8_t mode,
                 uint8_t speed,
                 uint8_t color,
                 uint8_t count) {
    uint8_t params[4] = { mode, speed, color, count };
    struct r502_ack_packet ack;
    /* Сканер GROW R502-F всегда возвращает ACK на команду 0x35 */
    return r502_send_command(uart_dev, R502_CMD_AURA_LED, params, sizeof(params), &ack, 300);
}

int r502_get_image(const struct device *uart_dev) {
    return r502_send_command(uart_dev, R502_CMD_GET_IMAGE, NULL, 0, NULL, R502_GET_IMAGE_TIMEOUT_MS);
}

int r502_image_to_tz(const struct device *uart_dev, uint8_t buffer_id) {
    uint8_t param = buffer_id;
    return r502_send_command(uart_dev, R502_CMD_IMAGE_TO_TZ, &param, 1, NULL, R502_PROCESS_TIMEOUT_MS);
}

int r502_search(const struct device *uart_dev,
                uint8_t buffer_id,
                uint16_t start_page,
                uint16_t page_num,
                uint16_t *found_page,
                uint16_t *score) {
    uint8_t params[5] = {
        buffer_id,
        (uint8_t)(start_page >> 8),
        (uint8_t)(start_page & 0xFF),
        (uint8_t)(page_num >> 8),
        (uint8_t)(page_num & 0xFF)
    };
    struct r502_ack_packet ack;
    int ret = r502_send_command(uart_dev, R502_CMD_SEARCH, params, sizeof(params), &ack, R502_PROCESS_TIMEOUT_MS);
    if (ret == R502_ACK_OK && ack.data_len >= 4) {
        if (found_page) {
            *found_page = (((uint16_t)ack.data[0]) << 8) | ack.data[1];
        }
        if (score) {
            *score = (((uint16_t)ack.data[2]) << 8) | ack.data[3];
        }
    }
    return ret;
}

int r502_reg_model(const struct device *uart_dev) {
    return r502_send_command(uart_dev, R502_CMD_REG_MODEL, NULL, 0, NULL, R502_PROCESS_TIMEOUT_MS);
}

int r502_store_char(const struct device *uart_dev, uint8_t buffer_id, uint16_t page_id) {
    uint8_t params[3] = {
        buffer_id,
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xFF)
    };
    return r502_send_command(uart_dev, R502_CMD_STORE_CHAR, params, sizeof(params), NULL, R502_PROCESS_TIMEOUT_MS);
}

int r502_delete_char(const struct device *uart_dev, uint16_t page_id, uint16_t count) {
    uint8_t params[4] = {
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xFF),
        (uint8_t)(count >> 8),
        (uint8_t)(count & 0xFF)
    };
    return r502_send_command(uart_dev, R502_CMD_DELETE_CHAR, params, sizeof(params), NULL, 1000);
}

int r502_empty(const struct device *uart_dev) {
    return r502_send_command(uart_dev, R502_CMD_EMPTY, NULL, 0, NULL, 2000);
}

int r502_get_template_count(const struct device *uart_dev, uint16_t *count) {
    struct r502_ack_packet ack;
    int ret = r502_send_command(uart_dev, R502_CMD_TEMPLATE_COUNT, NULL, 0, &ack, 500);
    if (ret == R502_ACK_OK && ack.data_len >= 2) {
        if (count) {
            *count = (((uint16_t)ack.data[0]) << 8) | ack.data[1];
        }
    }
    return ret;
}

int r502_handshake(const struct device *uart_dev) {
    return r502_send_command(uart_dev, R502_CMD_HANDSHAKE, NULL, 0, NULL, 500);
}
