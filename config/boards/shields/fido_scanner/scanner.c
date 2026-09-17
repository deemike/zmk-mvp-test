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

static const struct device *uart_dev;
static const struct device *touch_dev;

static atomic_t enroll_target_slot = ATOMIC_INIT(-1);
static enum scanner_state current_scanner_state = SCANNER_STATE_IDLE;
static uint16_t enrolled_templates_count = 0;

/* Колбэк UART по прерываниям RX: исключительно быстрый, без блокирующих операций и логов */
static void uart_cb(const struct device *dev, void *user_data) {
    uint8_t rx_buf[32];
    int recv_len;

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        recv_len = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
        if (recv_len <= 0) {
            break;
        }
        r502_driver_feed_rx(rx_buf, (size_t)recv_len);
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
    return 0;
}

enum scanner_state scanner_get_state(void) {
    return current_scanner_state;
}

/* Обработчик событий биометрической верификации */
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

    LOG_INF("=================================================");
    LOG_INF(">>> [ENROLLMENT] Registering Master Finger into Slot %u <<<", slot_id);
    LOG_INF(">>> Please press and HOLD your Master Finger firmly on sensor... <<<");
    LOG_INF("=================================================");

    /* Индикация фиолетовым дыханием: сканер ждет плотного прижатия пальца */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0xFF, R502_LED_COLOR_PURPLE, 0);
    k_msleep(200);

    /* Ожидание и захват снимка высокого качества (до 15 секунд) */
    int64_t capture_deadline = k_uptime_get() + 15000;
    bool image_ok = false;

    while (k_uptime_get() < capture_deadline) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            LOG_INF(">>> Master Fingerprint image captured successfully! <<<");
            image_ok = true;
            break;
        } else if (ret == R502_ACK_NO_FINGER) {
            /* Датчик активен, палец еще не прижат плотно */
        } else if (ret == -ETIMEDOUT) {
            LOG_WRN("GetImage timeout, checking link health...");
        }
        k_msleep(50);
    }

    if (!image_ok) {
        LOG_WRN("Enrollment: Failed to capture image (timeout)");
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_RED, 2);
        k_msleep(1500);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return -ETIMEDOUT;
    }

    /* Индикация: быстрый синий блик (обработка) */
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_BLUE, 1);
    k_msleep(30);
    r502_driver_flush_rx();

    /* Шаг 1: Извлечение характеристик в CharBuffer1 */
    ret = r502_image_to_tz(uart_dev, 1);
    if (ret != R502_ACK_OK) {
        LOG_ERR("Img2Tz (Buffer 1) failed: 0x%02X (poor quality, try pressing flatter)", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(1500);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return ret;
    }

    /* Шаг 2: Извлечение характеристик в CharBuffer2 для синтеза модели */
    k_msleep(30);
    r502_driver_flush_rx();
    r502_image_to_tz(uart_dev, 2);

    /* Шаг 3: Синтез модели отпечатка (PS_RegModel) */
    k_msleep(30);
    r502_driver_flush_rx();
    LOG_INF("Synthesizing fingerprint template (PS_RegModel)...");
    ret = r502_reg_model(uart_dev);
    LOG_INF("RegModel return code: 0x%02X", ret);

    /* Шаг 4: Сохранение шаблона в энергонезависимую Flash-память */
    k_msleep(30);
    r502_driver_flush_rx();
    LOG_INF("Storing Master Finger model into Flash Slot %u...", slot_id);
    ret = r502_store_char(uart_dev, 1, slot_id);
    if (ret == R502_ACK_OK) {
        /* Обновляем точное количество шаблонов прямо из памяти сенсора */
        uint16_t confirmed_count = 0;
        k_msleep(50);
        r502_driver_flush_rx();
        if (r502_get_template_count(uart_dev, &confirmed_count) == R502_ACK_OK) {
            enrolled_templates_count = confirmed_count;
        } else {
            enrolled_templates_count = (slot_id + 1 > enrolled_templates_count) ? (slot_id + 1) : enrolled_templates_count;
        }

        LOG_INF("=================================================");
        LOG_INF(">>> SUCCESS! Master Finger registered in Slot %u! <<<", slot_id);
        LOG_INF(">>> Enrolled templates in Flash library: %u <<<", enrolled_templates_count);
        LOG_INF(">>> Scanner is now in VERIFICATION MODE! <<<");
        LOG_INF("=================================================");
        /* Ровный зеленый свет на 3 секунды в знак полного успеха */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        k_msleep(3000);
    } else {
        LOG_ERR("StoreChar failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(2000);
    }

    /* Ждем, пока пользователь уберет палец после завершения */
    int64_t done_deadline = k_uptime_get() + 3000;
    while (k_uptime_get() < done_deadline) {
        if (r502_get_image(uart_dev) == R502_ACK_NO_FINGER) {
            break;
        }
        k_msleep(100);
    }
    r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
    current_scanner_state = SCANNER_STATE_IDLE;
    return ret;
}

/* Выполнение верификации пальца (Verification FSM) */
static void do_verify_finger(void) {
    int ret = -1;
    uint16_t found_page = 0;
    uint16_t score = 0;

    current_scanner_state = SCANNER_STATE_VERIFYING;

    LOG_INF(">>> [VERIFY] Finger detected, scanning... <<<");

    /* Мгновенная визуальная индикация: синий блик */
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_BLUE, 1);
    k_msleep(50);

    /* Захват изображения отпечатка (до 3 секунд пока палец прижимают) */
    int64_t verify_deadline = k_uptime_get() + 3000;
    bool image_ok = false;
    while (k_uptime_get() < verify_deadline) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            LOG_INF("Fingerprint image captured!");
            image_ok = true;
            break;
        }
        k_msleep(40);
    }

    if (!image_ok) {
        LOG_WRN("GetImage failed or finger lifted too fast (code 0x%02X)", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(1000);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return;
    }

    /* Очищаем буфер от устаревших ответов GetImage перед конвертацией */
    k_msleep(20);
    r502_driver_flush_rx();

    /* Извлечение характеристик в CharBuffer1 */
    ret = r502_image_to_tz(uart_dev, 1);
    if (ret != R502_ACK_OK) {
        LOG_WRN("Img2Tz failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(1000);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return;
    }

    /* Поиск по базе сохраненных отпечатков (слоты 0 - 100) */
    k_msleep(20);
    r502_driver_flush_rx();
    ret = r502_search(uart_dev, 1, 0, 100, &found_page, &score);
    if (ret == R502_ACK_OK) {
        /* УСПЕХ: Отпечаток найден в базе -> Зеленый свет на 1.5 сек */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        LOG_INF("=================================================");
        LOG_INF(">>> [ZMK_FIDO_AUTH] ACCESS GRANTED! Slot: %u, Score: %u <<<", found_page, score);
        LOG_INF("=================================================");
        scanner_on_auth_event(true, found_page, score);
        k_msleep(1500);
    } else {
        /* НЕУДАЧА: Нераспознанный отпечаток -> Красный свет на 1.5 сек */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        LOG_WRN("=================================================");
        LOG_WRN(">>> [ZMK_FIDO_AUTH] ACCESS DENIED (No match, code 0x%02X) <<<", ret);
        LOG_WRN("=================================================");
        scanner_on_auth_event(false, 0, 0);
        k_msleep(1500);
    }

    /* Выключение подсветки */
    r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
    current_scanner_state = SCANNER_STATE_IDLE;
}

/* Главный поток управления сканером */
static void scanner_thread_func(void *p1, void *p2, void *p3) {
    uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));
    touch_dev = DEVICE_DT_GET(TOUCH_GPIO_NODE);

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART1 device is not ready!");
        return;
    }

    /* Инициализация драйвера */
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

    /* Пауза 3500 мс: емкостной сенсор R502-F требует время на калибровку матрицы и сброс микроконтроллера */
    LOG_INF("Waiting for R502-F sensor boot and calibration (3500ms)...");
    k_msleep(3500);

    /* Сброс возможных ошибок линии во время включения питания сенсора (0x0C BREAK/FRAMING) */
    r502_uart_health_check(uart_dev);
    r502_driver_flush_rx();

    /* Принудительное включение белой пульсации для проверки TX линии */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0xFF, R502_LED_COLOR_WHITE, 0);
    k_msleep(150);

    /* Проверка связи с R502-F (до 5 попыток через нативный UART1 D6=TX, D7=RX @ 57600) */
    bool connected = false;
    uint16_t t_count = 0;
    for (int attempt = 1; attempt <= 5; attempt++) {
        r502_uart_health_check(uart_dev);
        r502_driver_flush_rx();
        int count_ret = r502_get_template_count(uart_dev, &t_count);
        if (count_ret == R502_ACK_OK) {
            connected = true;
            enrolled_templates_count = t_count;
            LOG_INF("=================================================");
            LOG_INF(">>> R502-F Connected! Enrolled templates in library: %u <<<", t_count);
            LOG_INF("=================================================");
            break;
        }
        LOG_WRN("Template count (cmd 0x1D) attempt %d/5 failed (ret=0x%02X), retrying...", attempt, count_ret);
        k_msleep(400);
    }

    if (connected) {
        /* При успешном старте: включаем постоянный зеленый свет на 1 сек и выключаем подсветку (Standby) */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        k_msleep(1000);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
    } else {
        LOG_WRN("Biometric scanner running in unlinked mode (waiting for touch)");
    }

    LOG_INF("Dixo Keyboard biometric scanner ready (Standby, LED OFF)!");

    bool last_touch = false;
    uint32_t idle_ticks = 0;

    while (1) {
        /* Проверяем, поступил ли запрос на регистрацию отпечатка извне */
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
            /* Переход LOW -> HIGH на пине D5: антидребезг 50 мс */
            k_msleep(50);
            if (gpio_pin_get(touch_dev, TOUCH_PIN) > 0) {
                last_touch = true;
                LOG_INF("Touch detected on Pin D5!");

                /* Если база пуста -> автоматически обучаем Мастер-палец в Слот 0 */
                if (enrolled_templates_count == 0) {
                    LOG_INF("Library is empty! Enrolling Master Finger into Slot 0...");
                    do_enroll_finger(0);
                } else {
                    /* Запуск верификации */
                    do_verify_finger();

                    /* Проверяем жест долгого удержания (3 секунды) для перерегистрации Slot 0 */
                    uint32_t hold_ms = 0;
                    while (is_finger_present() && hold_ms < 3000) {
                        k_msleep(100);
                        hold_ms += 100;
                        if (hold_ms == 2000) {
                            /* Подсказка пользователю: фиолетовое мигание перед сбросом */
                            r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x20, R502_LED_COLOR_PURPLE, 2);
                        }
                    }

                    if (hold_ms >= 3000) {
                        LOG_INF("Hold 3s detected! Resetting Slot 0 and re-enrolling Master Finger...");
                        r502_empty(uart_dev);
                        enrolled_templates_count = 0;
                        k_msleep(300);
                        do_enroll_finger(0);
                    }
                }
                idle_ticks = 0;
                continue;
            }
        } else if (!is_touched && last_touch) {
            /* Палец убран с датчика */
            last_touch = false;
            LOG_INF("Finger lifted from sensor");
        }

        /* Каждые 3 секунды выводим живой статус пина D5 в консоль */
        if (!last_touch && (idle_ticks % 30 == 0)) {
            LOG_INF("Heartbeat: Standby. Pin D5 level = %d, templates = %u", raw_pin, enrolled_templates_count);
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
