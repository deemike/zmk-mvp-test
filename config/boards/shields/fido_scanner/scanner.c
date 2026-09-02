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
static struct gpio_callback touch_cb_data;
static struct k_sem touch_sem;

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
                LOG_DBG("ACK RX: PID=0x%02X, Code=0x%02X, Len=%u",
                        packet.pid, packet.confirmation_code, packet.length);
                r502_driver_notify_ack(&packet);
            }
        }
    }
}

/* Колбэк GPIO касания Touch Pin */
static void touch_gpio_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    if (pins & BIT(TOUCH_PIN)) {
        k_sem_give(&touch_sem);
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

    /* Ждем касания (таймаут 10 секунд) */
    uint32_t wait_ms = 0;
    while (wait_ms < 10000) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            break;
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
        if (ret == R502_ACK_NO_FINGER || !is_finger_present()) {
            break;
        }
        k_msleep(100);
    }

    /* Шаг 2: Индикация фиолетовым миганием, ожидание 2-го касания */
    LOG_INF("Step 2/2: Place the SAME finger again...");
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x15, R502_LED_COLOR_PURPLE, 0);

    wait_ms = 0;
    while (wait_ms < 10000) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            break;
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
    int ret;
    uint16_t found_page = 0;
    uint16_t score = 0;

    current_scanner_state = SCANNER_STATE_VERIFYING;

    /* Индикация: быстрое синее мигание при начале сканирования */
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_BLUE, 2);
    k_msleep(60);

    /* Захват изображения отпечатка с ожиданием контакта (до 1.5 сек) */
    uint32_t wait_ms = 0;
    while (wait_ms < 1500) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            break;
        }
        k_msleep(50);
        wait_ms += 50;
    }

    if (ret != R502_ACK_OK) {
        LOG_DBG("GetImage: no finger captured (0x%02X)", ret);
        r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0x15, R502_LED_COLOR_BLUE, 0);
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

    /* Поиск по базе сохраненных отпечатков (слоты 0 - 99) */
    ret = r502_search(uart_dev, 1, 0, 100, &found_page, &score);
    if (ret == R502_ACK_OK) {
        /* УСПЕХ: Отпечаток найден в базе */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        scanner_on_auth_event(true, found_page, score);
        k_msleep(1500);
    } else {
        /* НЕУДАЧА: Отпечаток не распознан или не зарегистрирован */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        scanner_on_auth_event(false, 0, 0);
        k_msleep(1500);
    }

    /* Ожидание снятия пальца */
    wait_finger_release(3000);

    /* Возврат в фоновую индикацию */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0x15, R502_LED_COLOR_BLUE, 0);
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
    k_sem_init(&touch_sem, 0, 1);

    /* Настройка прерываний UART RX */
    uart_irq_callback_set(uart_dev, uart_cb);
    uart_irq_rx_enable(uart_dev);

    /* Настройка GPIO Touch Pin (D5) */
    if (device_is_ready(touch_dev)) {
        int err = gpio_pin_configure(touch_dev, TOUCH_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
        if (err == 0) {
            gpio_init_callback(&touch_cb_data, touch_gpio_cb, BIT(TOUCH_PIN));
            gpio_add_callback(touch_dev, &touch_cb_data);
            gpio_pin_interrupt_configure(touch_dev, TOUCH_PIN, GPIO_INT_EDGE_RISING);
            LOG_INF("Touch Sensing GPIO on Pin D5 configured successfully");
        } else {
            LOG_WRN("Failed to configure Touch GPIO: %d", err);
        }
    } else {
        LOG_WRN("Touch GPIO device not ready, touch sensing disabled");
    }

    /* Пауза для стабилизации питания сенсора и USB-консоли */
    LOG_INF("Waiting for R502-F sensor boot (3s)...");
    k_msleep(3000);

    /* Сбрасываем ложные прерывания, возникшие при переходных процессах подачи питания */
    k_sem_reset(&touch_sem);

    /* Проверка подключения сканера и количества сохраненных шаблонов */
    uint16_t tmpl_count = 0;
    int ret = r502_get_template_count(uart_dev, &tmpl_count);
    if (ret == R502_ACK_OK) {
        enrolled_templates_count = tmpl_count;
        LOG_INF("R502-F sensor connected! Enrolled templates in Flash: %u", enrolled_templates_count);
    } else {
        LOG_WRN("Could not read template count (code 0x%02X). Sensor may be booting...", ret);
    }

    /* Установка фоновой плавной синей подсветки */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0x15, R502_LED_COLOR_BLUE, 0);
    LOG_INF("Dixo Keyboard biometric scanner ready!");

    while (1) {
        /* Проверяем, поступил ли запрос на регистрацию отпечатка */
        atomic_val_t target_slot = atomic_set(&enroll_target_slot, -1);
        if (target_slot >= 0) {
            do_enroll_finger((uint16_t)target_slot);
            continue;
        }

        /* Ожидание сигнала касания по прерыванию GPIO (или тайм-аут 1 секунда) */
        int sem_res = k_sem_take(&touch_sem, K_MSEC(1000));

        /* Проверяем аппаратное касание (по семафору или высокому уровню пина D5) */
        if (sem_res == 0 || is_finger_present()) {
            LOG_INF("Touch event detected! Starting processing...");

            /* Если количество шаблонов еще неизвестно (было 0 из-за таймаута при старте), перепроверяем у сенсора */
            if (enrolled_templates_count == 0) {
                uint16_t count = 0;
                if (r502_get_template_count(uart_dev, &count) == R502_ACK_OK) {
                    enrolled_templates_count = count;
                    LOG_INF("Refreshed enrolled templates from sensor: %u", enrolled_templates_count);
                }
            }

            /* Если в базе действительно нет ни одного пальца — запускаем регистрацию Слота 0 */
            if (enrolled_templates_count == 0) {
                LOG_INF("No fingerprints in database! Automatically enrolling Master Finger (Slot 0)...");
                do_enroll_finger(0);
            } else {
                do_verify_finger();
            }

            /* Сбрасываем накопившиеся события семафора после завершения операции */
            k_sem_reset(&touch_sem);
        }
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
