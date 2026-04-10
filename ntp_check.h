/**
 * @file ntp_check.h
 * @brief NTP synchronisation check via an NTP mode-6 control message.
 *
 * Queries the local NTP daemon on 127.0.0.1:123 using an NTP mode-6
 * (control) message and returns whether the daemon currently has a
 * synchronised clock.  This is used by the CAN timestamp sender to
 * avoid injecting unsynchronised time onto the bus.
 */

#ifndef _NTP_CHECK_H_
#define _NTP_CHECK_H_

#include <stdbool.h>

/**
 * @brief Check whether the local NTP daemon is synchronised.
 *
 * Sends a read-variables (opcode 2) NTP mode-6 control message to
 * 127.0.0.1:123 and interprets the response.  The clock is considered
 * synchronised when:
 *   - the leap-indicator bits are not both set (LI ≠ 3),
 *   - the response contains valid stratum, dispersion, delay and poll
 *     fields.
 *
 * The function is non-blocking with a 1-second select() timeout.
 *
 * @return @c true if the NTP clock is synchronised, @c false otherwise
 *         (including socket errors or timeout).
 */
bool ntp_check(void);

#endif
