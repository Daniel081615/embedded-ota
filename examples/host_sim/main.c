/* examples/host_sim/main.c
 *
 * End-to-end host simulation of the OTA receive path. Injects three mock
 * vtables (flash / sys / net) into the portable core and drives a leaf node
 * (OTA_ROLE_METER) through a full update:
 *
 *     UPDATE_CHILD_REQ  ->  STORE_CHILD_REQ x N  ->  CRC verify  ->  TRIGGER
 *
 * Proves the Shared/ota core compiles and runs on a PC under gcc with zero
 * platform dependencies. Exit code 0 = pass.
 */
#include <stdio.h>
#include <string.h>

#include "ota_scheduler.h"
#include "flash_service.h"
#include "ota_protocol.h"
#include "mock_flash.h"

/* ---- mock IOtaSys_t : monotonic millisecond tick ---- */
static uint32_t s_tick;
static uint32_t sys_get_tick(void) { return ++s_tick; }
static const IOtaSys_t SYS = { sys_get_tick };

/* ---- mock IOtaNet_t : print upstream traffic, capture the trigger ---- */
static int s_triggered;

static int32_t net_send_up(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    (void)p;
    printf("    upstream <- cmd=0x%02X len=%u\n", cmd, n);
    return 0;
}
static int32_t net_send_down(uint8_t id, uint8_t cmd, const uint8_t *p, uint16_t n)
{
    (void)id; (void)cmd; (void)p; (void)n;   /* leaf node never forwards */
    return 0;
}
static void net_trigger(uint8_t bank, uint32_t payload_size, uint32_t crc,
                        uint32_t img_size, uint32_t version, uint8_t node_id)
{
    (void)img_size; (void)node_id;
    s_triggered = 1;
    printf("    TriggerProxy: bank=%u payload=%u crc=0x%08X ver=%u"
           "  (leaf -> would enter bootloader)\n",
           bank, payload_size, crc, version);
}
static const IOtaNet_t NET = { net_send_up, net_send_down, net_trigger };

int main(void)
{
    uint8_t  image[256];
    uint32_t fw_size = (uint32_t)sizeof(image);
    uint32_t fw_crc;
    uint8_t  meta[1 + OTA_PROTO_UPDATE_META_LEN];   /* node_id + 20-byte UPDATE_META */
    uint32_t off;
    uint32_t i;

    printf("== embedded-ota host_sim ==\n");

    /* 1. A fake firmware image (4-byte aligned) and its CRC. */
    for (i = 0u; i < fw_size; i++) image[i] = (uint8_t)(i * 7u + 1u);
    fw_crc = MockFlash_Crc32(image, fw_size);
    printf("firmware: %u bytes, crc=0x%08X\n", fw_size, fw_crc);

    /* 2. Wire up the portable core with mock drivers. */
    MockFlash_Reset();
    FlashService_Init(&MOCK_FLASH);
    OtaScheduler_Init(OTA_ROLE_METER, &NET, &SYS);

    /* 3. UPDATE_CHILD_REQ: node_id prefix + standard 20-byte UPDATE_META. */
    meta[0] = 1u;   /* downstream node id */
    OtaProto_EncodeUpdateMeta(&meta[1], fw_size, fw_size, fw_crc, 1u);
    printf("-> UPDATE_CHILD_REQ\n");
    OtaScheduler_OnReceivePacket(OTA_CMD_UPDATE_CHILD_REQ, meta, (uint16_t)sizeof(meta));
    OtaScheduler_Task();   /* erase target bank, move to RECV_DATA */

    if (!OtaScheduler_IsActive()) {
        printf("FAIL: scheduler idle after UPDATE (rejected meta?)\n");
        return 1;
    }

    /* 4. Stream the firmware as STORE_CHILD_REQ chunks: [offset 4B][data <=88B]. */
    printf("-> STORE chunks\n");
    for (off = 0u; off < fw_size; ) {
        uint8_t  pkt[OTA_CHUNK_SIZE];
        uint32_t dlen = fw_size - off;
        if (dlen > 88u) dlen = 88u;
        pkt[0] = (uint8_t)(off);
        pkt[1] = (uint8_t)(off >> 8);
        pkt[2] = (uint8_t)(off >> 16);
        pkt[3] = (uint8_t)(off >> 24);
        memcpy(&pkt[4], &image[off], dlen);
        OtaScheduler_OnReceivePacket(OTA_CMD_STORE_CHILD_REQ, pkt, (uint16_t)(4u + dlen));
        OtaScheduler_Task();
        off += dlen;
    }

    /* 5. After the last chunk the core verifies CRC inline and returns to IDLE. */
    if (OtaScheduler_IsActive()) {
        printf("FAIL: still active after last chunk (CRC mismatch / short write)\n");
        return 1;
    }

    /* 6. TRIGGER -> TriggerProxy (a leaf would now jump to the bootloader). */
    printf("-> TRIGGER_CHILD_REQ\n");
    {
        uint8_t dummy = 0u;
        OtaScheduler_OnReceivePacket(OTA_CMD_TRIGGER_CHILD_REQ, &dummy, 1u);
    }
    if (!s_triggered) {
        printf("FAIL: TriggerProxy was not called\n");
        return 1;
    }

    printf("PASS: %u-byte firmware stored, CRC verified, trigger fired.\n", fw_size);
    return 0;
}
