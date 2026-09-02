#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "scanner.h"
#include "r502_protocol.h"
#include "r502_driver.h"

LOG_MODULE_REGISTER(scanner_fido, LOG_LEVEL_DBG);

#define SCANNER_STACK_SIZE 2048
#define SCANNER_PRIORITY   7

#define TOUCH_GPIO_NODE    DT_NODELABEL(gpio0)
#define TOUCH_PIN          5 /* XIAO BLE D5 = P0.05 */

K_THREAD_STACK_DEFINE(scanner_stack_area, SCANNER_STACK_SIZE);
static struct k_thread scanner_thread_data;

static struct r502_parser parser;
static const struct device *uart_dev;
static const struct device *touch_dev;

static atomic_t enroll_target_slot = ATOMIC_INIT(-1);
static enum scanner_state current_scanner_state = SCANNER_STATE_IDLE;
static uint16_t enrolled_templates_count = 0;

/* Колбэк UART по прерываниям RX */
static void uart_cb(const struct device *dev, void *user_data) {
    uint8_t rx_buf[16];
    int recv_len;

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        recv_len = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
        if (recv_len <= 0) {
            break;
        }

        for (int i = 0; i < recv_len; i++) {
            struct r502_ack_packet packet;
            if (r502_parser_feed_byte(&parser, rx_buf[i], &packet)) {
                LOG_INF("ACK RX: PID=0x%02X, Code=0x%02X, Len=%u",
                        packet.pid, packet.confirmation_code, packet.length);
                r502_driver_notify_ack(&packet);
            }
        }
    }
}

/* Проверка присутствия пальца на датчике */
static bool is_finger_present(void) {
    if (touch_dev && device_is_ready(touch_dev)) {
        return gpio_pin_get(touch_dev, TOUCH_PIN) > 0;
    }
    return false;
}

/* Ожидание снятия пальца с датчика */
static void wait_finger_release(uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (!is_finger_present()) {
            break;
        }
        k_msleep(50);
        elapsed += 50;
    }
}

/* Публичное API запроса регистрации пальца */
int scanner_start_enroll(uint16_t slot_id) {
    if (slot_id >= 100) {
        return -EINVAL;
    }
    atomic_set(&enroll_target_slot, (atomic_val_t)slot_id);
    k_sem_give(&touch_sem);
    return 0;
}

enum scanner_state scanner_get_state(void) {
    return current_scanner_state;
}

/* Обработчик событий биометрической верификации (Вариант А: USB лог / статус) */
void scanner_on_auth_event(bool success, uint16_t slot_id, uint16_t score) {
    if (success) {
        LOG_INF("=================================================");
        LOG_INF(">>> [ZMK_FIDO_AUTH] User verified! Slot: %d, Score: %d <<<", slot_id, score);
        LOG_INF("=================================================");
    } else {
        LOG_WRN("=================================================");
        LOG_WRN(">>> [ZMK_FIDO_AUTH] Verification failed (No match) <<<");
        LOG_WRN("=================================================");
    }
}

/* Выполнение регистрации нового пальца (Enrollment FSM) */
static int do_enroll_finger(uint16_t slot_id) {
    int ret;
    current_scanner_state = SCANNER_STATE_ENROLLING;

    LOG_INF(">>> ENROLLMENT MODE: Preparing to enroll Finger into Slot %u <<<", slot_id);

    /* Шаг 1: Индикация фиолетовым дыханием, ожидание 1-го касания */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0x15, R502_LED_COLOR_PURPLE, 0);
    LOG_INF("Step 1/2: Place finger on sensor...");

    /* Ждем касания (таймаут 10 секунд или 3 ошибки связи) */
    uint32_t wait_ms = 0;
    int timeout_count = 0;
    while (wait_ms < 10000) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            break;
        } else if (ret < 0) {
            timeout_count++;
            if (timeout_count >= 3) {
                LOG_WRN("UART timeout: sensor not responding");
                break;
            }
        }
        k_msleep(100);
        wait_ms += 100;
    }

    if (ret != R502_ACK_OK) {
        LOG_WRN("Enrollment timeout: Finger not detected");
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_RED, 2);
        k_msleep(1000);
        current_scanner_state = SCANNER_STATE_IDLE;
        return -ETIMEDOUT;
    }

    /* Преобразование 1-го снимка в характеристики CharBuffer1 */
    ret = r502_image_to_tz(uart_dev, 1);
    if (ret != R502_ACK_OK) {
        LOG_ERR("Img2Tz (Buffer 1) failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_RED, 2);
        k_msleep(1000);
        current_scanner_state = SCANNER_STATE_IDLE;
        return ret;
    }

    /* Сигнализируем успех 1-го считывания быстрым зеленым бликом */
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_GREEN, 1);
    LOG_INF("Step 1 OK! Please LIFT your finger...");
    k_msleep(500);

    /* Ждем, пока палец снимут */
    while (1) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_NO_FINGER || !is_finger_present() || ret < 0) {
            break;
        }
        k_msleep(100);
    }

    /* Шаг 2: Индикация фиолетовым миганием, ожидание 2-го касания */
    LOG_INF("Step 2/2: Place the SAME finger again...");
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x15, R502_LED_COLOR_PURPLE, 0);

    wait_ms = 0;
    timeout_count = 0;
    while (wait_ms < 10000) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            break;
        } else if (ret < 0) {
            timeout_count++;
            if (timeout_count >= 3) {
                LOG_WRN("UART timeout on step 2: sensor not responding");
                break;
            }
        }
        k_msleep(100);
        wait_ms += 100;
    }

    if (ret != R502_ACK_OK) {
        LOG_WRN("Enrollment timeout on step 2");
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_RED, 2);
        k_msleep(1000);
        current_scanner_state = SCANNER_STATE_IDLE;
        return -ETIMEDOUT;
    }

    /* Преобразование 2-го снимка в CharBuffer2 */
    ret = r502_image_to_tz(uart_dev, 2);
    if (ret != R502_ACK_OK) {
        LOG_ERR("Img2Tz (Buffer 2) failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_RED, 2);
        k_msleep(1000);
        current_scanner_state = SCANNER_STATE_IDLE;
        return ret;
    }

    /* Шаг 3: Объединение моделей */
    LOG_INF("Combining models (PS_RegModel)...");
    ret = r502_reg_model(uart_dev);
    if (ret != R502_ACK_OK) {
        LOG_ERR("RegModel failed (prints do not match): 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(2000);
        current_scanner_state = SCANNER_STATE_IDLE;
        return ret;
    }

    /* Шаг 4: Сохранение шаблона в Flash */
    LOG_INF("Storing model into Slot %u...", slot_id);
    ret = r502_store_char(uart_dev, 1, slot_id);
    if (ret == R502_ACK_OK) {
        LOG_INF("=================================================");
        LOG_INF(">>> ENROLLMENT SUCCESSFUL! Stored in Slot %u <<<", slot_id);
        LOG_INF("=================================================");
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        enrolled_templates_count++;
        k_msleep(2000);
    } else {
        LOG_ERR("StoreChar failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(2000);
    }

    wait_finger_release(2000);
    current_scanner_state = SCANNER_STATE_IDLE;
    return ret;
}

/* Выполнение верификации пальца (Verification FSM) */
static void do_verify_finger(void) {
    int ret = -1;
    uint16_t found_page = 0;
    uint16_t score = 0;

    current_scanner_state = SCANNER_STATE_VERIFYING;

    /* Индикация: быстрое синее мигание при начале сканирования */
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_BLUE, 2);

    /* Захват изображения отпечатка (до 3 попыток по 60 мс) */
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            break;
        }
        k_msleep(60);
    }

    if (ret != R502_ACK_OK) {
        LOG_WRN("GetImage failed or no finger detected (code 0x%02X)", ret);
        current_scanner_state = SCANNER_STATE_IDLE;
        return;
    }

    /* Извлечение характеристик в CharBuffer1 */
    ret = r502_image_to_tz(uart_dev, 1);
    if (ret != R502_ACK_OK) {
        LOG_WRN("Img2Tz failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x15, R502_LED_COLOR_RED, 2);
        k_msleep(1000);
        current_scanner_state = SCANNER_STATE_IDLE;
        return;
    }

    /* Поиск по базе сохраненных отпечатков (слоты 0 - 100) */
    ret = r502_search(uart_dev, 1, 0, 100, &found_page, &score);
    if (ret == R502_ACK_OK) {
        /* УСПЕХ: Отпечаток найден в базе */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        LOG_INF("=================================================");
        LOG_INF(">>> [ZMK_FIDO_AUTH] User verified! Slot: %u, Score: %u <<<", found_page, score);
        LOG_INF("=================================================");
        scanner_on_auth_event(true, found_page, score);
        k_msleep(1500);
    } else {
        /* НЕУДАЧА: Отпечаток не распознан или не зарегистрирован */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        LOG_WRN("=================================================");
        LOG_WRN(">>> [ZMK_FIDO_AUTH] Verification failed (No match, code 0x%02X) <<<", ret);
        LOG_WRN("=================================================");
        scanner_on_auth_event(false, 0, 0);
        k_msleep(1500);
    }

    /* Ожидание снятия пальца */
    wait_finger_release(2000);
    current_scanner_state = SCANNER_STATE_IDLE;
}

/* Главный поток управления сканером */
static void scanner_thread_func(void *p1, void *p2, void *p3) {
    uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart0));
    touch_dev = DEVICE_DT_GET(TOUCH_GPIO_NODE);

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART0 device is not ready!");
        return;
    }

    /* Инициализация протокола и драйвера */
    r502_parser_init(&parser);
    r502_driver_init();

    /* Настройка прерываний UART RX */
    uart_irq_callback_set(uart_dev, uart_cb);
    uart_irq_rx_enable(uart_dev);

    /* Настройка GPIO Touch Pin (D5) */
    if (device_is_ready(touch_dev)) {
        int err = gpio_pin_configure(touch_dev, TOUCH_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
        if (err == 0) {
            LOG_INF("Touch Sensing GPIO on Pin D5 configured (PULL_DOWN)");
        } else {
            LOG_WRN("Failed to configure Touch GPIO: %d", err);
        }
    } else {
        LOG_WRN("Touch GPIO device not ready");
    }

    /* Пауза для стабилизации питания сенсора и USB-консоли */
    LOG_INF("Waiting for R502-F sensor boot (3s)...");
    k_msleep(3000);

    int pin_lvl = -1;
    if (touch_dev && device_is_ready(touch_dev)) {
        pin_lvl = gpio_pin_get(touch_dev, TOUCH_PIN);
    }
    LOG_INF("Initial Touch GPIO level at boot: %d", pin_lvl);

    LOG_INF("Dixo Keyboard biometric scanner ready!");

    uint32_t idle_ticks = 0;
    bool last_touch = false;

    while (1) {
        /* Проверяем, поступил ли запрос на регистрацию отпечатка */
        atomic_val_t target_slot = atomic_set(&enroll_target_slot, -1);
        if (target_slot >= 0) {
            do_enroll_finger((uint16_t)target_slot);
            continue;
        }

        /* Опрос пина D5 с программным антидребезгом */
        int raw_pin = -1;
        if (touch_dev && device_is_ready(touch_dev)) {
            raw_pin = gpio_pin_get(touch_dev, TOUCH_PIN);
        }

        bool is_touched = (raw_pin > 0);

        if (is_touched && !last_touch) {
            /* Переход LOW -> HIGH: антидребезг 50 мс */
            k_msleep(50);
            if (gpio_pin_get(touch_dev, TOUCH_PIN) > 0) {
                last_touch = true;
                LOG_INF("Touch detected on Pin D5! Starting verification...");
                do_verify_finger();
                idle_ticks = 0;
            }
        } else if (!is_touched && last_touch) {
            /* Палец убран с датчика */
            last_touch = false;
            LOG_INF("Finger lifted from sensor");
        }

        /* Каждые 2 секунды (20 тиков по 100 мс) в покое шлем сиреневую пульсацию */
        if (!last_touch && (idle_ticks % 20 == 0)) {
            uint8_t cmd_purple_breathe[] = {
                0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
                0x01, 0x00, 0x07,
                0x35, 0x01, 0xFF, 0x03, 0x00,
                0x01, 0x40
            };
            for (size_t i = 0; i < sizeof(cmd_purple_breathe); i++) {
                uart_poll_out(uart_dev, cmd_purple_breathe[i]);
            }
        }

        idle_ticks++;
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
