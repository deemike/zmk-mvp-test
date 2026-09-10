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

/* Колбэк UART по прерываниям RX */
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
        LOG_INF("--> RAW UART RX: %02X (len %d)", rx_buf[0], recv_len);
        r502_driver_feed_rx(rx_buf, recv_len);
    }
}

/* Проверка присутствия пальца на датчике */
static bool is_finger_present(void) {
    if (touch_dev && device_is_ready(touch_dev)) {
        if (gpio_pin_get(touch_dev, TOUCH_PIN) > 0) {
            return true;
        }
    }
    if (uart_dev && device_is_ready(uart_dev)) {
        return (r502_get_image(uart_dev) == R502_ACK_OK);
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
    LOG_INF(">>> ENROLLMENT MODE: Preparing to enroll Finger into Slot %u <<<", slot_id);
    LOG_INF("=================================================");

    /* Шаг 1: Индикация фиолетовым дыханием */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0xFF, R502_LED_COLOR_PURPLE, 0);
    LOG_INF("Step 1/2: Please place and hold your finger firmly on the sensor...");

    k_msleep(250);

    /* Ожидание и захват 1-го снимка (до 12 секунд).
     * Опрашиваем сканер напрямую: когда палец плотно прижат, GetImage вернет R502_ACK_OK.
     */
    uint32_t wait_ms = 0;
    bool image1_ok = false;

    while (wait_ms < 12000) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            LOG_INF("Step 1: Fingerprint image 1 captured successfully!");
            image1_ok = true;
            break;
        }
        k_msleep(100);
        wait_ms += 100;
    }

    if (!image1_ok) {
        LOG_WRN("Enrollment Step 1: Failed to capture image (timeout or bad read)");
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_RED, 2);
        k_msleep(1500);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return -ETIMEDOUT;
    }

    /* Преобразование 1-го снимка в характеристики CharBuffer1 */
    ret = r502_image_to_tz(uart_dev, 1);
    if (ret != R502_ACK_OK) {
        LOG_ERR("Img2Tz (Buffer 1) failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(1500);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return ret;
    }

    /* Сигнализируем успех 1-го считывания: постоянный зеленый свет на 700 мс */
    r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
    k_msleep(700);
    r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
    LOG_INF("Step 1 OK! Please LIFT your finger from the sensor...");

    /* Ждем, пока пользователь снимет палец со сканера */
    uint32_t release_wait = 0;
    while (release_wait < 6000) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_NO_FINGER) {
            LOG_INF("Finger lifted!");
            break;
        }
        k_msleep(100);
        release_wait += 100;
    }
    k_msleep(400);

    /* Шаг 2: Индикация фиолетовым миганием, ожидание 2-го касания */
    LOG_INF("Step 2/2: Place the SAME finger again firmly on the sensor...");
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x20, R502_LED_COLOR_PURPLE, 0);

    wait_ms = 0;
    bool image2_ok = false;

    while (wait_ms < 12000) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            LOG_INF("Step 2: Fingerprint image 2 captured successfully!");
            image2_ok = true;
            break;
        }
        k_msleep(100);
        wait_ms += 100;
    }

    if (!image2_ok) {
        LOG_WRN("Enrollment Step 2: Failed to capture image (timeout)");
        r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_RED, 2);
        k_msleep(1500);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return -ETIMEDOUT;
    }

    /* Преобразование 2-го снимка в CharBuffer2 */
    ret = r502_image_to_tz(uart_dev, 2);
    if (ret != R502_ACK_OK) {
        LOG_ERR("Img2Tz (Buffer 2) failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(1500);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
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
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return ret;
    }

    /* Шаг 4: Сохранение шаблона в Flash */
    LOG_INF("Storing model into Slot %u...", slot_id);
    ret = r502_store_char(uart_dev, 1, slot_id);
    if (ret == R502_ACK_OK) {
        LOG_INF("=================================================");
        LOG_INF(">>> ENROLLMENT SUCCESSFUL! Master finger stored in Slot %u <<<", slot_id);
        LOG_INF("=================================================");
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        enrolled_templates_count++;
        k_msleep(3000);
    } else {
        LOG_ERR("StoreChar failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(2000);
    }

    /* Ждем, пока пользователь уберет палец после завершения */
    release_wait = 0;
    while (release_wait < 3000) {
        if (r502_get_image(uart_dev) == R502_ACK_NO_FINGER) {
            break;
        }
        k_msleep(100);
        release_wait += 100;
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

    /* Мгновенная визуальная индикация: синий блик */
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x10, R502_LED_COLOR_BLUE, 1);

    /* Небольшая пауза 100 мс */
    k_msleep(100);

    /* Захват изображения отпечатка (до 2.5 секунд пока палец прижимают) */
    uint32_t wait_ms = 0;
    bool image_ok = false;
    while (wait_ms < 2500) {
        ret = r502_get_image(uart_dev);
        if (ret == R502_ACK_OK) {
            LOG_INF("Fingerprint image captured successfully!");
            image_ok = true;
            break;
        }
        k_msleep(60);
        wait_ms += 60;
    }

    if (!image_ok) {
        LOG_WRN("GetImage failed or finger lifted too fast (code 0x%02X)", ret);
        /* Ошибка считывания -> Красный свет на 1 сек */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(1000);
        r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
        current_scanner_state = SCANNER_STATE_IDLE;
        return;
    }

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
    ret = r502_search(uart_dev, 1, 0, 100, &found_page, &score);
    if (ret == R502_ACK_OK) {
        /* УСПЕХ: Отпечаток найден в базе -> Зеленый свет на 1.5 сек */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_GREEN, 0);
        LOG_INF("=================================================");
        LOG_INF(">>> [ZMK_FIDO_AUTH] User verified! Slot: %u, Score: %u <<<", found_page, score);
        LOG_INF("=================================================");
        scanner_on_auth_event(true, found_page, score);
        k_msleep(1500);
    } else {
        /* НЕУДАЧА: Нераспознанный отпечаток -> Красный свет на 1.5 сек */
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        LOG_WRN("=================================================");
        LOG_WRN(">>> [ZMK_FIDO_AUTH] Verification failed (No match, code 0x%02X) <<<", ret);
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

    /* Принудительное включение белой пульсации для проверки TX линии */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0xFF, R502_LED_COLOR_WHITE, 0);

    /* Проверка связи с R502-F (до 5 попыток через нативный UART1 D6=TX, D7=RX @ 9600) */
    bool connected = false;
    uint16_t t_count = 0;
    for (int attempt = 1; attempt <= 5; attempt++) {
        /* Отправляем команду Handshake (0x53) для пробуждения интерфейса сканера */
        int hs_ret = r502_handshake(uart_dev);
        LOG_INF("Handshake (cmd 0x53) attempt %d/5 returned: 0x%02X", attempt, hs_ret);
        k_msleep(50);

        int count_ret = r502_get_template_count(uart_dev, &t_count);
        if (count_ret == R502_ACK_OK) {
            connected = true;
            enrolled_templates_count = t_count;
            LOG_INF("=================================================");
            LOG_INF(">>> R502-F Connected! Enrolled templates: %u <<<", t_count);
            LOG_INF("=================================================");
            break;
        }
        LOG_WRN("Template count (cmd 0x1D) attempt %d/5 failed (ret=0x%02X), retrying...", attempt, count_ret);
        k_msleep(500);
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

        /* Опрос пина D5 */
        int raw_pin = -1;
        if (touch_dev && device_is_ready(touch_dev)) {
            raw_pin = gpio_pin_get(touch_dev, TOUCH_PIN);
        }

        bool is_touched = false;
        if (raw_pin > 0) {
            is_touched = true;
        } else if (last_touch) {
            /* Если палец уже был прижат (last_touch == true), но D5 == 0,
             * опрашиваем UART, не снят ли палец */
            is_touched = (r502_get_image(uart_dev) == R502_ACK_OK);
        } else if (idle_ticks % 3 == 0) {
            /* Режим ожидания: периодический опрос сканера по UART каждые 300 мс (фоллбэк при неактивном/неподключенном D5) */
            is_touched = (r502_get_image(uart_dev) == R502_ACK_OK);
        }

        if (is_touched && !last_touch) {
            bool confirmed = false;
            if (raw_pin > 0) {
                /* Переход LOW -> HIGH на пине D5: антидребезг 50 мс */
                k_msleep(50);
                if (gpio_pin_get(touch_dev, TOUCH_PIN) > 0) {
                    confirmed = true;
                }
            } else {
                /* Касание подтверждено опросом по UART */
                confirmed = true;
            }

            if (confirmed) {
                last_touch = true;
                LOG_INF("Touch detected%s!", (raw_pin > 0) ? " on Pin D5" : " via UART polling");

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
