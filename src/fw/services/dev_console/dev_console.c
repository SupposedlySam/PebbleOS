/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! Remote console endpoint (0xCAFE). Runs a PebbleOS console-command string sent
//! over the Pebble protocol (BLE / CloudPebble relay) through the normal prompt
//! dispatcher, and returns the command's captured output on the same endpoint.
//!
//! This is the "flash once, drive forever" affordance: a host can run any
//! console command (dart wasm, app launch, future psram/heap/peek...) and read
//! the result without USB, without a screen, and without reflashing. The work
//! runs on KernelBG (never the boot path), so it can't brick boot.

#include "console/prompt.h"
#include "pbl/services/comm_session/session.h"
#include "pbl/services/system_task.h"
#include "system/logging.h"

#include <string.h>

#define DEV_CONSOLE_ENDPOINT 0x4000
#define DEV_CONSOLE_RESP_MAX 1024

static CommSession *s_session;
static char s_cmd[PROMPT_BUFFER_SIZE_BYTES];
static size_t s_cmd_len;
static char s_resp[DEV_CONSOLE_RESP_MAX];
static size_t s_resp_len;
static PromptContext s_ctx;

//! Each prompt response line is appended (newline-terminated) to the reply.
static void prv_response_cb(const char *response) {
  size_t n = strlen(response);
  if (s_resp_len + n + 1 < DEV_CONSOLE_RESP_MAX) {
    memcpy(s_resp + s_resp_len, response, n);
    s_resp_len += n;
    s_resp[s_resp_len++] = '\n';
  }
}

//! Fires when the command completes (sync or async) — send the captured output.
static void prv_complete_cb(void) {
  if (!s_session) {
    return;
  }
  if (s_resp_len == 0) {
    static const char kOk[] = "(ok)\n";
    memcpy(s_resp, kOk, sizeof(kOk) - 1);
    s_resp_len = sizeof(kOk) - 1;
  }
  comm_session_send_data(s_session, DEV_CONSOLE_ENDPOINT, (uint8_t *)s_resp,
                         s_resp_len, COMM_SESSION_DEFAULT_TIMEOUT);
  s_session = NULL;
}

//! Runs on KernelBG: dispatch the command through the prompt with our capturing
//! context. prv_complete_cb sends the reply (handles both sync + async commands).
static void prv_run_command(void *data) {
  s_resp_len = 0;
  s_ctx.response_callback = prv_response_cb;
  s_ctx.command_complete_callback = prv_complete_cb;
  memcpy(s_ctx.buffer, s_cmd, s_cmd_len);
  s_ctx.buffer[s_cmd_len] = '\0';
  s_ctx.write_index = s_cmd_len;
  prompt_context_execute(&s_ctx);
}

void dev_console_protocol_msg_callback(CommSession *session, const uint8_t *data,
                                       size_t length) {
  if (prompt_command_is_executing() || s_session) {
    return;  // one command at a time; host should retry
  }
  s_session = session;
  s_cmd_len = (length < sizeof(s_cmd) - 1) ? length : (sizeof(s_cmd) - 1);
  memcpy(s_cmd, data, s_cmd_len);
  // Offload off the BT receive context (can't send with bt_lock held).
  if (!system_task_add_callback(prv_run_command, NULL)) {
    s_session = NULL;
  }
}
