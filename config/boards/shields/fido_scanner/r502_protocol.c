#include "r502_protocol.h"
#include <string.h>

void r502_parser_init(struct r502_parser *parser) {
    if (!parser) return;
    memset(parser, 0, sizeof(*parser));
    parser->state = R502_PARSE_HEADER1;
}

bool r502_parser_feed_byte(struct r502_parser *parser, uint8_t byte, struct r502_ack_packet *out_packet) {
    if (!parser) return false;

    switch (parser->state) {
    case R502_PARSE_HEADER1:
        if (byte == R502_HEADER_1) {
            parser->state = R502_PARSE_HEADER2;
        }
        break;

    case R502_PARSE_HEADER2:
        if (byte == R502_HEADER_2) {
            parser->state = R502_PARSE_ADDR;
            parser->addr_bytes_read = 0;
        } else if (byte != R502_HEADER_1) {
            parser->state = R502_PARSE_HEADER1;
        }
        break;

    case R502_PARSE_ADDR:
        parser->addr_bytes_read++;
        if (parser->addr_bytes_read >= 4) {
            parser->state = R502_PARSE_PID;
        }
        break;

    case R502_PARSE_PID:
        parser->current_packet.pid = byte;
        parser->calc_sum = byte;
        parser->state = R502_PARSE_LEN_H;
        break;

    case R502_PARSE_LEN_H:
        parser->length = ((uint16_t)byte) << 8;
        parser->calc_sum += byte;
        parser->state = R502_PARSE_LEN_L;
        break;

    case R502_PARSE_LEN_L:
        parser->length |= byte;
        parser->calc_sum += byte;
        parser->current_packet.length = parser->length;

        /* Длина пакета должна содержать минимум 1 байт кода подтверждения + 2 байта контрольной суммы */
        if (parser->length < 3 || parser->length > (R502_MAX_PAYLOAD + 2)) {
            /* Ошибочная длина пакета, сброс парсера */
            parser->state = R502_PARSE_HEADER1;
            break;
        }

        parser->payload_bytes_read = 0;
        parser->state = R502_PARSE_CONFIRM_CODE;
        break;

    case R502_PARSE_CONFIRM_CODE:
        parser->current_packet.confirmation_code = byte;
        parser->calc_sum += byte;
        parser->payload_bytes_read = 1;
        parser->current_packet.data_len = 0;

        /* Если полезная нагрузка состояла только из Confirmation Code (length == 3) */
        if (parser->length == 3) {
            parser->state = R502_PARSE_CKSUM_H;
        } else {
            parser->state = R502_PARSE_DATA;
        }
        break;

    case R502_PARSE_DATA:
        if (parser->current_packet.data_len < R502_MAX_PAYLOAD) {
            parser->current_packet.data[parser->current_packet.data_len++] = byte;
        }
        parser->calc_sum += byte;
        parser->payload_bytes_read++;

        /* Проверяем, прочитали ли все байты полезной нагрузки (length - 2) */
        if (parser->payload_bytes_read >= (parser->length - 2)) {
            parser->state = R502_PARSE_CKSUM_H;
        }
        break;

    case R502_PARSE_CKSUM_H:
        parser->rx_sum = ((uint16_t)byte) << 8;
        parser->state = R502_PARSE_CKSUM_L;
        break;

    case R502_PARSE_CKSUM_L:
        parser->rx_sum |= byte;
        parser->current_packet.checksum = parser->rx_sum;

        /* Сброс состояния для следующего пакета */
        parser->state = R502_PARSE_HEADER1;

        if (parser->rx_sum == parser->calc_sum) {
            parser->current_packet.valid = true;
            if (out_packet) {
                *out_packet = parser->current_packet;
            }
            return true;
        } else {
            /* Ошибка контрольной суммы */
            parser->current_packet.valid = false;
            return false;
        }

    default:
        parser->state = R502_PARSE_HEADER1;
        break;
    }

    return false;
}

int r502_build_command(uint8_t *buffer, size_t buf_size, uint8_t cmd, const uint8_t *params, size_t param_len) {
    /* Минимальный размер пакета: Header(2) + Addr(4) + PID(1) + Len(2) + Cmd(1) + Params(N) + Checksum(2) */
    size_t total_len = 9 + 1 + param_len + 2;
    if (buf_size < total_len) {
        return -1;
    }

    uint16_t pkg_len = 1 + param_len + 2; /* Cmd + params + 2 bytes checksum */

    /* Header */
    buffer[0] = R502_HEADER_1;
    buffer[1] = R502_HEADER_2;

    /* Address (Default 0xFFFFFFFF) */
    buffer[2] = 0xFF;
    buffer[3] = 0xFF;
    buffer[4] = 0xFF;
    buffer[5] = 0xFF;

    /* PID */
    buffer[6] = R502_PID_COMMAND;

    /* Length */
    buffer[7] = (uint8_t)(pkg_len >> 8);
    buffer[8] = (uint8_t)(pkg_len & 0xFF);

    /* Command */
    buffer[9] = cmd;

    /* Parameters */
    if (param_len > 0 && params != NULL) {
        memcpy(&buffer[10], params, param_len);
    }

    /* Checksum: sum of PID + Length + Cmd + Params */
    uint16_t sum = buffer[6] + buffer[7] + buffer[8] + buffer[9];
    for (size_t i = 0; i < param_len; i++) {
        sum += params[i];
    }

    buffer[10 + param_len] = (uint8_t)(sum >> 8);
    buffer[11 + param_len] = (uint8_t)(sum & 0xFF);

    return (int)total_len;
}
