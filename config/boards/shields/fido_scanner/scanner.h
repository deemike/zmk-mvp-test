#ifndef SCANNER_H_
#define SCANNER_H_

#include <stdint.h>
#include <stdbool.h>

/* Режимы работы сканера */
enum scanner_state {
    SCANNER_STATE_IDLE,
    SCANNER_STATE_VERIFYING,
    SCANNER_STATE_ENROLLING,
};

/* Инициировать регистрацию нового пальца в указанный слот (0-99) */
int scanner_start_enroll(uint16_t slot_id);

/* Текущее состояние сканера */
enum scanner_state scanner_get_state(void);

/* Колбэк события аутентификации */
void scanner_on_auth_event(bool success, uint16_t slot_id, uint16_t score);

#endif /* SCANNER_H_ */
