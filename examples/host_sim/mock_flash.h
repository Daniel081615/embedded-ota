/* examples/host_sim/mock_flash.h
 *
 * Host-side mock of the IFmcDriver_t flash port: a plain RAM array standing in
 * for MCU flash. Lets the OTA core run on a PC under gcc — no hardware needed.
 */
#ifndef MOCK_FLASH_H
#define MOCK_FLASH_H

#include <stdint.h>
#include "ota_flash_port.h"   /* IFmcDriver_t */

/* The mock flash driver instance to inject via FlashService_Init(). */
extern const IFmcDriver_t MOCK_FLASH;

/* Reset the simulated flash to the erased state (all 0xFF). Call before use. */
void MockFlash_Reset(void);

/* Standard CRC-32 (IEEE 802.3, poly 0xEDB88320). The mock's GetCRC32 uses the
 * same routine, so a caller can pre-compute the expected CRC of an image. */
uint32_t MockFlash_Crc32(const uint8_t *data, uint32_t len);

#endif /* MOCK_FLASH_H */
