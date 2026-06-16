#ifndef OTA_PROTOCOL_SHARED_H
#define OTA_PROTOCOL_SHARED_H

#include <stdint.h>

/*
 * 共用 OTA Protocol 層 — 與下游 Bootloader 之間的 payload 編碼格式
 * （Single Source of Truth）
 * ------------------------------------------------------------------
 * 純資料格式定義 + encode 函式，不含狀態機、不含 I/O。
 * 由 CENTER(Gateway) / METER(Concentrator) / RoomNode 端的
 * ota_center_proxy.c / ota_center_relay.c / ota_meter_proxy.c 共用，
 * 避免各自重複實作相同的 byte-packing（消除複製漂移）。
 *
 * 「跨裝置必須一致」的 wire 格式放這裡；無任何平台相依內容。
 *
 * 修改規則：任何一方變更 = 變更全體。
 *
 * 工具鏈：本檔需同時通過 Keil armclang 與 Atmel Studio arm-gcc，
 * 僅用純 C，勿加編譯器專屬語法。
 */

/* UPDATE_CHILD_REQ（統一 BL 格式，20 bytes，不含 node_id）長度 */
#define OTA_PROTO_UPDATE_META_LEN 20u

/* STORE_CHILD_REQ chunk 開頭的 offset 欄位長度 */
#define OTA_PROTO_CHUNK_HDR_LEN   4u

/*
 * 編碼 UPDATE_CHILD_REQ（UPDATE_META）20-byte payload：
 * [0-3]  payload_size     LE
 * [4-7]  fw_image_size    LE
 * [8-11] transport_crc32  LE
 * [12-15]version          LE
 * [16-19]meta_version=1   LE
 */
static inline void OtaProto_EncodeUpdateMeta(uint8_t out[OTA_PROTO_UPDATE_META_LEN],
                                              uint32_t payload_size,
                                              uint32_t fw_image_size,
                                              uint32_t transport_crc32,
                                              uint32_t version)
{
    out[ 0] = (uint8_t)(payload_size);
    out[ 1] = (uint8_t)(payload_size >>  8);
    out[ 2] = (uint8_t)(payload_size >> 16);
    out[ 3] = (uint8_t)(payload_size >> 24);
    out[ 4] = (uint8_t)(fw_image_size);
    out[ 5] = (uint8_t)(fw_image_size >>  8);
    out[ 6] = (uint8_t)(fw_image_size >> 16);
    out[ 7] = (uint8_t)(fw_image_size >> 24);
    out[ 8] = (uint8_t)(transport_crc32);
    out[ 9] = (uint8_t)(transport_crc32 >>  8);
    out[10] = (uint8_t)(transport_crc32 >> 16);
    out[11] = (uint8_t)(transport_crc32 >> 24);
    out[12] = (uint8_t)(version);
    out[13] = (uint8_t)(version >>  8);
    out[14] = (uint8_t)(version >> 16);
    out[15] = (uint8_t)(version >> 24);
    out[16] = 1u; out[17] = 0u; out[18] = 0u; out[19] = 0u;  /* meta_version = 1 */
}

/*
 * 編碼 STORE_CHILD_REQ chunk 開頭的 4-byte offset（LE）。
 * 韌體資料本身由呼叫端緊接著寫入 out[4..]。
 */
static inline void OtaProto_EncodeChunkOffset(uint8_t out[OTA_PROTO_CHUNK_HDR_LEN],
                                               uint32_t offset)
{
    out[0] = (uint8_t)(offset);
    out[1] = (uint8_t)(offset >>  8);
    out[2] = (uint8_t)(offset >> 16);
    out[3] = (uint8_t)(offset >> 24);
}

#endif /* OTA_PROTOCOL_SHARED_H */
