/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! Debug Log GATT service: pages the in-RAM firmware log ring out over plain GATT
//! (no encryption, no pairing, no PPoG). This is the always-available observability
//! channel -- it works even when a connection never reaches PPoG (the exact state we
//! could not previously diagnose).
//!
//! Service 0000c0de-…  ("code")
//!   char 0000c0d1 (write): set read offset (LE uint32, logical bytes from oldest)
//!   char 0000c0d2 (read) : returns [LE uint32 total_count][up to 240 log bytes @offset]
//! Mac side: write offset=0, read -> total + chunk; advance offset by chunk len; repeat
//! until offset >= total. See pebble_ble.py read_logs().

#include <string.h>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "system/log_ring.h"

#define DEBUG_LOG_CHUNK_MAX 240u

static uint32_t s_read_offset;

// 16-bit UUIDs (0xc0de/0xc0d1/0xc0d2), expanded via the SIG base. A 128-bit *service*
// UUID failed inside ble_gatts_start() on obelix and bricked the HCPU on boot; 16-bit
// matches the working fed9 service. Bleak readers using the full base form still match.
/* 0000c0de-... */
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(0xc0de);
/* 0000c0d1-... : control (write offset) */
static const ble_uuid16_t s_ctrl_uuid = BLE_UUID16_INIT(0xc0d1);
/* 0000c0d2-... : data (read) */
static const ble_uuid16_t s_data_uuid = BLE_UUID16_INIT(0xc0d2);

static int prv_ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  uint8_t b[4] = {0, 0, 0, 0};
  uint16_t copied = 0;
  ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), &copied);
  s_read_offset = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                  ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return 0;
}

static int prv_data_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  uint32_t count = log_ring_count();
  uint8_t hdr[4] = {(uint8_t)count, (uint8_t)(count >> 8),
                    (uint8_t)(count >> 16), (uint8_t)(count >> 24)};
  if (os_mbuf_append(ctxt->om, hdr, sizeof(hdr)) != 0) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  uint8_t chunk[DEBUG_LOG_CHUNK_MAX];
  uint32_t n = log_ring_read(s_read_offset, chunk, sizeof(chunk));
  if (n > 0 && os_mbuf_append(ctxt->om, chunk, n) != 0) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &s_svc_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
      {
        .uuid = &s_ctrl_uuid.u,
        .access_cb = prv_ctrl_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
      }, {
        .uuid = &s_data_uuid.u,
        .access_cb = prv_data_access,
        .flags = BLE_GATT_CHR_F_READ,
      }, {
        0, /* no more characteristics */
      },
    },
  },
  {
    0, /* no more services */
  },
};

void debug_log_service_init(void) {
  if (ble_gatts_count_cfg(s_svcs) != 0) {
    return;
  }
  ble_gatts_add_svcs(s_svcs);
}
