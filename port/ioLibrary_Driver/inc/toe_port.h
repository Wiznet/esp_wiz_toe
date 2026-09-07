/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Neutral (plain-C) port helpers used by wiznet_toe.c.
 *
 * wiznet_toe.c must NOT include FreeRTOS/ESP/POSIX headers: it includes the
 * ioLibrary "socket.h", whose socket()/close()/recv()/send() names would clash
 * with POSIX/newlib declarations pulled in transitively. So the RTOS-coupled
 * bits (yield, time) live in wizchip_spi_esp.c and are reached through these
 * two plain prototypes.
 */
#ifndef TOE_PORT_H
#define TOE_PORT_H

#include <stdint.h>

/* Block the calling task ~1 ms (FreeRTOS vTaskDelay), yielding to others. */
void toe_yield_1ms(void);

/* Free-running microsecond counter (esp_timer). Used to randomize the ephemeral
 * port in wiztoe_connect(), and to bound the graceful-shutdown wait in close()
 * so a dead link cannot stall the calling task. Wraps roughly every 71 minutes;
 * callers must compare with unsigned subtraction. */
uint32_t toe_time_us(void);

#endif /* TOE_PORT_H */
