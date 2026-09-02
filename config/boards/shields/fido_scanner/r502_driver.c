#include "r502_driver.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_DECLARE(scanner_fido, LOG_LEVEL_DBG);

static struct k_sem ack_sem;
static struct k_mutex r502_lock;
static struct r502_ack_packet last_ack;

void r502_driver_init(void) {
    k_sem_init(&ack_sem, 0, 1);
    k_mutex_init(&r502_lock);
}

void r502_driver_notify_ack(const struct r502_ack_packet *packet) {
    if (!packet) return;
    last_ack = *packet;
    k_sem_give(&ack_sem);
}

int r502_send_command(const struct device *uart_dev,
                      uint8_t cmd,
                      const uint8_t *params,
                      size_t param_len,
                      struct r502_ack_packet *out_ack,
                      k_timeout_t timeout) {
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

    /* Сбрасываем старый семафор, если он был взведен */
    k_sem_reset(&ack_sem);

    /* Отправка данных по UART */
    for (int i = 0; i < pkg_len; i++) {
        uart_poll_out(uart_dev, tx_buf[i]);
    }

    /* Ожидание ответа с таймаутом */
    int ret = k_sem_take(&ack_sem, timeout);
    if (ret != 0) {
        LOG_WRN("Timeout waiting for ACK on cmd 0x%02X", cmd);
        k_mutex_unlock(&r502_lock);
        return -ETIMEDOUT;
    }

    if (!last_ack.valid) {
        LOG_ERR("Received invalid ACK packet (bad checksum)");
        k_mutex_unlock(&r502_lock);
        return -EIO;
    }

    if (out_ack) {
        *out_ack = last_ack;
    }

    int code = last_ack.confirmation_code;
    k_mutex_unlock(&r502_lock);

    return code;
}

int r502_set_led(const struct device *uart_dev,
                 uint8_t mode,
                 uint8_t speed,
                 uint8_t color,
                 uint8_t count) {
    uint8_t params[4] = { mode, speed, color, count };
    return r502_send_command(uart_dev, R502_CMD_AURA_LED, params, sizeof(params), NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
}

int r502_get_image(const struct device *uart_dev) {
    return r502_send_command(uart_dev, R502_CMD_GET_IMAGE, NULL, 0, NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
}

int r502_image_to_tz(const struct device *uart_dev, uint8_t buffer_id) {
    uint8_t param = buffer_id;
    return r502_send_command(uart_dev, R502_CMD_IMAGE_TO_TZ, &param, 1, NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
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
    int ret = r502_send_command(uart_dev, R502_CMD_SEARCH, params, sizeof(params), &ack, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
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
    return r502_send_command(uart_dev, R502_CMD_REG_MODEL, NULL, 0, NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
}

int r502_store_char(const struct device *uart_dev, uint8_t buffer_id, uint16_t page_id) {
    uint8_t params[3] = {
        buffer_id,
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xFF)
    };
    return r502_send_command(uart_dev, R502_CMD_STORE_CHAR, params, sizeof(params), NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
}

int r502_delete_char(const struct device *uart_dev, uint16_t page_id, uint16_t count) {
    uint8_t params[4] = {
        (uint8_t)(page_id >> 8),
        (uint8_t)(page_id & 0xFF),
        (uint8_t)(count >> 8),
        (uint8_t)(count & 0xFF)
    };
    return r502_send_command(uart_dev, R502_CMD_DELETE_CHAR, params, sizeof(params), NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
}

int r502_empty(const struct device *uart_dev) {
    return r502_send_command(uart_dev, R502_CMD_EMPTY, NULL, 0, NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
}

int r502_get_template_count(const struct device *uart_dev, uint16_t *count) {
    struct r502_ack_packet ack;
    int ret = r502_send_command(uart_dev, R502_CMD_TEMPLATE_COUNT, NULL, 0, &ack, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
    if (ret == R502_ACK_OK && ack.data_len >= 2) {
        if (count) {
            *count = (((uint16_t)ack.data[0]) << 8) | ack.data[1];
        }
    }
    return ret;
}

int r502_handshake(const struct device *uart_dev) {
    return r502_send_command(uart_dev, R502_CMD_HANDSHAKE, NULL, 0, NULL, K_MSEC(R502_DEFAULT_TIMEOUT_MS));
}
