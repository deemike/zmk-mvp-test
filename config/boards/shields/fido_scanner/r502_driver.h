#ifndef R502_DRIVER_H_
#define R502_DRIVER_H_

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include "r502_protocol.h"

/* Таймауты ожидания ответа по умолчанию */
#define R502_DEFAULT_TIMEOUT_MS     1000
#define R502_CAPTURE_TIMEOUT_MS     2000

/* Инициализация подсистемы драйвера сканера */
void r502_driver_init(void);

/* Оповещение драйвера о получении полного ACK пакета (вызывается из обработчика входящего потока) */
void r502_driver_notify_ack(const struct r502_ack_packet *packet);

/* Базовая функция синхронной отправки команды и ожидания ответа */
int r502_send_command(const struct device *uart_dev,
                      uint8_t cmd,
                      const uint8_t *params,
                      size_t param_len,
                      struct r502_ack_packet *out_ack,
                      k_timeout_t timeout);

/* Управление круговой подсветкой (Aura LED) */
int r502_set_led(const struct device *uart_dev,
                 uint8_t mode,
                 uint8_t speed,
                 uint8_t color,
                 uint8_t count);

/* Сбор отпечатка с сенсора (PS_GetImage) */
int r502_get_image(const struct device *uart_dev);

/* Преобразование изображения в файл характеристик (PS_GenChar/Img2Tz) */
int r502_image_to_tz(const struct device *uart_dev, uint8_t buffer_id);

/* Поиск совпадения отпечатка в Flash-библиотеке (PS_Search) */
int r502_search(const struct device *uart_dev,
                uint8_t buffer_id,
                uint16_t start_page,
                uint16_t page_num,
                uint16_t *found_page,
                uint16_t *score);

/* Объединение CharBuffer1 и CharBuffer2 в один шаблон (PS_RegModel) */
int r502_reg_model(const struct device *uart_dev);

/* Сохранение шаблона из CharBuffer в Flash-память (PS_StoreChar) */
int r502_store_char(const struct device *uart_dev, uint8_t buffer_id, uint16_t page_id);

/* Удаление шаблонов из Flash-памяти (PS_DeletChar) */
int r502_delete_char(const struct device *uart_dev, uint16_t page_id, uint16_t count);

/* Полная очистка Flash-памяти (PS_Empty) */
int r502_empty(const struct device *uart_dev);

/* Получение количества сохраненных шаблонов (PS_TemplateNum) */
int r502_get_template_count(const struct device *uart_dev, uint16_t *count);

/* Рукопожатие со сканером (PS_Handshake) */
int r502_handshake(const struct device *uart_dev);

#endif /* R502_DRIVER_H_ */
