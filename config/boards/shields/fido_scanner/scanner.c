#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <hal/nrf_uarte.h>
#include <hal/nrf_gpio.h>

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
        r502_driver_feed_rx(rx_buf, recv_len);
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
    LOG_INF(">>> ENROLLMENT MODE: Preparing to enroll Finger into Slot %u <<<", slot_id);
    LOG_INF("=================================================");

    /* Шаг 1: Индикация фиолетовым дыханием */
    r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0xFF, R502_LED_COLOR_PURPLE, 0);
    LOG_INF("Step 1/2: Please place and hold your finger firmly on the sensor...");

    /* Пауза 250 мс для стабилизации прижатия пальца к стеклу сканера */
    k_msleep(250);

    /* Ожидание и захват 1-го снимка (до 10 секунд) */
    uint32_t wait_ms = 0;
    bool image1_ok = false;

    while (wait_ms < 10000) {
        if (is_finger_present()) {
            ret = r502_get_image(uart_dev);
            if (ret == R502_ACK_OK) {
                LOG_INF("Step 1: Fingerprint image 1 captured successfully!");
                image1_ok = true;
                break;
            }
        }
        k_msleep(200);
        wait_ms += 200;
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

    /* Сигнализируем успех 1-го считывания зеленым бликом */
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x15, R502_LED_COLOR_GREEN, 1);
    LOG_INF("Step 1 OK! Please LIFT your finger...");

    /* Ждем, пока палец снимут */
    while (is_finger_present()) {
        k_msleep(80);
    }
    k_msleep(500);

    /* Шаг 2: Индикация фиолетовым миганием, ожидание 2-го касания */
    LOG_INF("Step 2/2: Place the SAME finger again...");
    r502_set_led(uart_dev, R502_LED_MODE_FLASHING, 0x20, R502_LED_COLOR_PURPLE, 0);

    /* Ждем повторного касания */
    wait_ms = 0;
    while (!is_finger_present() && wait_ms < 10000) {
        k_msleep(100);
        wait_ms += 100;
    }

    /* Пауза для стабилизации прижатия */
    k_msleep(250);

    wait_ms = 0;
    bool image2_ok = false;

    while (wait_ms < 10000) {
        if (is_finger_present()) {
            ret = r502_get_image(uart_dev);
            if (ret == R502_ACK_OK) {
                LOG_INF("Step 2: Fingerprint image 2 captured successfully!");
                image2_ok = true;
                break;
            }
        }
        k_msleep(200);
        wait_ms += 200;
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
        k_msleep(2500);
    } else {
        LOG_ERR("StoreChar failed: 0x%02X", ret);
        r502_set_led(uart_dev, R502_LED_MODE_ON, 0x00, R502_LED_COLOR_RED, 0);
        k_msleep(2000);
    }

    wait_finger_release(1000);
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

    /* Небольшая пауза 150 мс для полного прилегания пальца к сенсору */
    k_msleep(150);

    /* Захват изображения отпечатка (до 1.5 сек пока палец прижат) */
    uint32_t wait_ms = 0;
    bool image_ok = false;
    while (wait_ms < 1500) {
        if (is_finger_present()) {
            ret = r502_get_image(uart_dev);
            if (ret == R502_ACK_OK) {
                LOG_INF("Fingerprint image captured successfully!");
                image_ok = true;
                break;
            }
        }
        k_msleep(100);
        wait_ms += 100;
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

struct probe_cfg {
    const char *desc;
    uint32_t tx_pin;
    uint32_t rx_pin;
    nrf_uarte_baudrate_t baud_reg;
    uint32_t baud_num;
};

static void apply_uart_hw_config(uint32_t tx_pin, uint32_t rx_pin, nrf_uarte_baudrate_t baud) {
    nrf_uarte_disable(NRF_UARTE1);
    nrf_uarte_txrx_pins_disconnect(NRF_UARTE1);

    /* Настройка пинов: TX на выход (HIGH в покое), RX на вход с подтяжкой PULL_UP */
    nrf_gpio_cfg_output(tx_pin);
    nrf_gpio_pin_set(tx_pin);
    nrf_gpio_cfg_input(rx_pin, NRF_GPIO_PIN_PULLUP);

    nrf_uarte_txrx_pins_set(NRF_UARTE1, tx_pin, rx_pin);
    nrf_uarte_baudrate_set(NRF_UARTE1, baud);

    nrf_uarte_event_clear(NRF_UARTE1, NRF_UARTE_EVENT_ENDRX);
    nrf_uarte_event_clear(NRF_UARTE1, NRF_UARTE_EVENT_ENDTX);
    nrf_uarte_event_clear(NRF_UARTE1, NRF_UARTE_EVENT_ERROR);

    nrf_uarte_enable(NRF_UARTE1);
    nrf_uarte_task_trigger(NRF_UARTE1, NRF_UARTE_TASK_STARTRX);
}

static bool probe_scanner_connection(const struct device *dev) {
    static const struct probe_cfg configs[] = {
        { "Xiao D7 (P1.12)=TX -> Sensor RX (Brown), Xiao D6 (P1.11)=RX <- Sensor TX (Yellow)",
          NRF_GPIO_PIN_MAP(1, 12), NRF_GPIO_PIN_MAP(1, 11), NRF_UARTE_BAUDRATE_57600, 57600 },
        { "Xiao D6 (P1.11)=TX -> Sensor RX, Xiao D7 (P1.12)=RX <- Sensor TX",
          NRF_GPIO_PIN_MAP(1, 11), NRF_GPIO_PIN_MAP(1, 12), NRF_UARTE_BAUDRATE_57600, 57600 },
        { "Xiao D7 (P1.12)=TX -> Sensor RX, Xiao D6 (P1.11)=RX <- Sensor TX",
          NRF_GPIO_PIN_MAP(1, 12), NRF_GPIO_PIN_MAP(1, 11), NRF_UARTE_BAUDRATE_115200, 115200 },
        { "Xiao D6 (P1.11)=TX -> Sensor RX, Xiao D7 (P1.12)=RX <- Sensor TX",
          NRF_GPIO_PIN_MAP(1, 11), NRF_GPIO_PIN_MAP(1, 12), NRF_UARTE_BAUDRATE_115200, 115200 },
        { "Xiao D7 (P1.12)=TX -> Sensor RX, Xiao D6 (P1.11)=RX <- Sensor TX",
          NRF_GPIO_PIN_MAP(1, 12), NRF_GPIO_PIN_MAP(1, 11), NRF_UARTE_BAUDRATE_9600, 9600 },
        { "Xiao D6 (P1.11)=TX -> Sensor RX, Xiao D7 (P1.12)=RX <- Sensor TX",
          NRF_GPIO_PIN_MAP(1, 11), NRF_GPIO_PIN_MAP(1, 12), NRF_UARTE_BAUDRATE_9600, 9600 },
    };

    LOG_INF("=================================================");
    LOG_INF(">>> Starting R502-F Auto-Detection Probe... <<<");
    LOG_INF("=================================================");

    for (size_t i = 0; i < ARRAY_SIZE(configs); i++) {
        const struct probe_cfg *cfg = &configs[i];
        LOG_INF("Probe [%u/%u]: Testing %s @ %u baud...", (uint32_t)(i + 1), (uint32_t)ARRAY_SIZE(configs), cfg->desc, cfg->baud_num);

        apply_uart_hw_config(cfg->tx_pin, cfg->rx_pin, cfg->baud_reg);
        k_msleep(30);

        /* 1. Проверяем команду Handshake (0x53) */
        int ret = r502_handshake(dev);
        if (ret == R502_ACK_OK) {
            LOG_INF("=================================================");
            LOG_INF(">>> [PROBE SUCCESS] Handshake confirmed! <<<");
            LOG_INF(">>> Connected: %s @ %u baud <<<", cfg->desc, cfg->baud_num);
            LOG_INF("=================================================");
            return true;
        }

        /* 2. Проверяем команду Template Count (0x1D) */
        uint16_t dummy_count = 0;
        ret = r502_get_template_count(dev, &dummy_count);
        if (ret == R502_ACK_OK) {
            LOG_INF("=================================================");
            LOG_INF(">>> [PROBE SUCCESS] Template count confirmed (%u templates)! <<<", dummy_count);
            LOG_INF(">>> Connected: %s @ %u baud <<<", cfg->desc, cfg->baud_num);
            LOG_INF("=================================================");
            enrolled_templates_count = dummy_count;
            return true;
        }

        /* 3. Проверяем команду Aura LED (0x35) */
        ret = r502_set_led(dev, R502_LED_MODE_BREATHING, 0xFF, R502_LED_COLOR_PURPLE, 0);
        if (ret == R502_ACK_OK) {
            LOG_INF("=================================================");
            LOG_INF(">>> [PROBE SUCCESS] Aura LED confirmed! <<<");
            LOG_INF(">>> Connected: %s @ %u baud <<<", cfg->desc, cfg->baud_num);
            LOG_INF("=================================================");
            return true;
        }
    }

    LOG_WRN("=================================================");
    LOG_WRN(">>> [PROBE FAILED] Sensor did not answer any UART permutation! <<<");
    LOG_WRN(">>> Reverting to default (Xiao D7=TX, D6=RX @ 57600 baud) <<<");
    LOG_WRN("=================================================");
    apply_uart_hw_config(NRF_GPIO_PIN_MAP(1, 12), NRF_GPIO_PIN_MAP(1, 11), NRF_UARTE_BAUDRATE_57600);
    return false;
}

/* Главный поток управления сканером */
static void scanner_thread_func(void *p1, void *p2, void *p3) {
    uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));
    touch_dev = DEVICE_DT_GET(TOUCH_GPIO_NODE);

    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART1 device is not ready!");
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

    /* Пауза 1000 мс: даем сенсору R502-F завершить калибровку емкостного датчика при подаче питания */
    LOG_INF("Waiting for R502-F sensor boot and calibration (1000ms)...");
    k_msleep(1000);

    /* Запуск автоматического поиска правильной распиновки D6/D7 и скорости UART */
    bool connected = probe_scanner_connection(uart_dev);

    if (connected) {
        /* Включаем сиреневую пульсацию (Aura LED Purple Breathing) для наглядного подтверждения */
        r502_set_led(uart_dev, R502_LED_MODE_BREATHING, 0xFF, R502_LED_COLOR_PURPLE, 0);

        uint16_t t_count = 0;
        int count_ret = r502_get_template_count(uart_dev, &t_count);
        if (count_ret == R502_ACK_OK) {
            enrolled_templates_count = t_count;
            LOG_INF("R502-F Ready! Enrolled templates in flash: %u", t_count);
            if (t_count > 0) {
                r502_set_led(uart_dev, R502_LED_MODE_OFF, 0x00, 0x00, 0);
            }
        }
    } else {
        LOG_WRN("Biometric scanner running in unlinked mode (waiting for touch)");
    }

    LOG_INF("Dixo Keyboard biometric scanner ready!");

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
            /* Переход LOW -> HIGH: антидребезг 50 мс */
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
