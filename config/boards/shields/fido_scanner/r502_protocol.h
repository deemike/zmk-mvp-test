#ifndef R502_PROTOCOL_H_
#define R502_PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Базовые параметры пакета R502-F */
#define R502_HEADER_1           0xEF
#define R502_HEADER_2           0x01
#define R502_DEFAULT_ADDR       0xFFFFFFFF

/* Идентификаторы типов пакетов (PID) */
#define R502_PID_COMMAND        0x01    /* Пакет команды от хоста к сканеру */
#define R502_PID_DATA           0x02    /* Пакет данных */
#define R502_PID_ACK            0x07    /* Пакет ответа (подтверждения) от сканера */
#define R502_PID_END_DATA       0x08    /* Завершающий пакет данных */

/* Опкоды команд (Instruction Codes) */
#define R502_CMD_GET_IMAGE      0x01    /* Сбор отпечатка в ImageBuffer */
#define R502_CMD_IMAGE_TO_TZ    0x02    /* Генерация характеристик из ImageBuffer в CharBuffer1/2 */
#define R502_CMD_MATCH          0x03    /* Точное сопоставление CharBuffer1 и CharBuffer2 */
#define R502_CMD_SEARCH         0x04    /* Поиск шаблона по библиотеке в Flash */
#define R502_CMD_REG_MODEL      0x05    /* Объединение характеристик в шаблон */
#define R502_CMD_STORE_CHAR     0x06    /* Сохранение шаблона в Flash память */
#define R502_CMD_LOAD_CHAR      0x07    /* Загрузка шаблона из Flash в CharBuffer */
#define R502_CMD_DELETE_CHAR    0x0C    /* Удаление шаблона (шаблонов) из Flash */
#define R502_CMD_EMPTY          0x0D    /* Полная очистка базы отпечатков */
#define R502_CMD_TEMPLATE_COUNT 0x1D    /* Запрос количества сохраненных шаблонов */
#define R502_CMD_CANCEL         0x30    /* Отмена текущей операции */
#define R502_CMD_AURA_LED       0x35    /* Управление круговой подсветкой (Aura LED) */
#define R502_CMD_HANDSHAKE      0x53    /* Рукопожатие (Handshake) */

/* Коды подтверждения (Confirmation Codes в ACK-пакетах) */
#define R502_ACK_OK                     0x00    /* Успешное выполнение */
#define R502_ACK_PACKET_ERR             0x01    /* Ошибка приема пакета */
#define R502_ACK_NO_FINGER              0x02    /* Палец не обнаружен на датчике */
#define R502_ACK_ENROLL_FAIL            0x03    /* Ошибка записи/сбора пальца */
#define R502_ACK_BAD_IMAGE_DISORDER     0x06    /* Слишком смазанное изображение */
#define R502_ACK_BAD_IMAGE_FEW_POINTS   0x07    /* Мало характерных точек (малая площадь) */
#define R502_ACK_NO_MATCH               0x08    /* Отпечатки не совпадают */
#define R502_ACK_NOT_FOUND              0x09    /* Совпадение в базе не найдено */
#define R502_ACK_MERGE_FAIL             0x0A    /* Не удалось объединить отпечатки (разные пальцы) */
#define R502_ACK_PAGE_OUT_OF_RANGE      0x0B    /* Номер ячейки выходит за границы памяти */
#define R502_ACK_READ_ERR               0x0C    /* Ошибка чтения шаблона из памяти */
#define R502_ACK_FLASH_ERR              0x18    /* Ошибка записи в Flash */

/* Режимы управления подсветкой (Aura LED Control Mode) */
#define R502_LED_MODE_BREATHING         0x01    /* Плавное дыхание */
#define R502_LED_MODE_FLASHING          0x02    /* Мигание */
#define R502_LED_MODE_ON                0x03    /* Постоянно включен */
#define R502_LED_MODE_OFF               0x04    /* Постоянно выключен */
#define R502_LED_MODE_GRADUAL_ON        0x05    /* Плавное включение */
#define R502_LED_MODE_GRADUAL_OFF       0x06    /* Плавное выключение */

/* Цвета подсветки Aura LED */
#define R502_LED_COLOR_RED              0x01    /* Красный */
#define R502_LED_COLOR_BLUE             0x02    /* Синий */
#define R502_LED_COLOR_PURPLE           0x03    /* Сиреневый / Пурпурный */
#define R502_LED_COLOR_GREEN            0x04    /* Зеленый */
#define R502_LED_COLOR_YELLOW           0x05    /* Желтый */
#define R502_LED_COLOR_CYAN             0x06    /* Голубой */
#define R502_LED_COLOR_WHITE            0x07    /* Белый */

/* Максимальный размер полезной нагрузки в ACK */
#define R502_MAX_PAYLOAD                32

/* Структура разобранного ACK пакета */
struct r502_ack_packet {
    uint8_t pid;
    uint16_t length;
    uint8_t confirmation_code;
    uint8_t data[R502_MAX_PAYLOAD];
    uint16_t data_len;
    uint16_t checksum;
    bool valid;
};

/* Состояния потокового конечного автомата парсера */
enum r502_parser_state {
    R502_PARSE_HEADER1,
    R502_PARSE_HEADER2,
    R502_PARSE_ADDR,
    R502_PARSE_PID,
    R502_PARSE_LEN_H,
    R502_PARSE_LEN_L,
    R502_PARSE_CONFIRM_CODE,
    R502_PARSE_DATA,
    R502_PARSE_CKSUM_H,
    R502_PARSE_CKSUM_L,
};

/* Контекст парсера */
struct r502_parser {
    enum r502_parser_state state;
    uint8_t addr_bytes_read;
    uint16_t length;
    uint16_t payload_bytes_read;
    uint16_t calc_sum;
    uint16_t rx_sum;
    struct r502_ack_packet current_packet;
};

/* Инициализация парсера */
void r502_parser_init(struct r502_parser *parser);

/* Потоковая передача байта в парсер. Возвращает true, если валидный пакет полностью собран */
bool r502_parser_feed_byte(struct r502_parser *parser, uint8_t byte, struct r502_ack_packet *out_packet);

/* Сборка команды в буфер для передачи. Возвращает длину собранного пакета */
int r502_build_command(uint8_t *buffer, size_t buf_size, uint8_t cmd, const uint8_t *params, size_t param_len);

#endif /* R502_PROTOCOL_H_ */
