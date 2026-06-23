/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! Remote console endpoint (0x4000). Runs a PebbleOS console-command string sent
//! over the Pebble protocol (BLE / CloudPebble relay) through the normal prompt
//! dispatcher, and returns the command's captured output.
//!
//! The "flash once, drive forever" affordance: a host runs any console command
//! (dart wasm, app launch, future psram/heap/peek...) and reads the result with
//! no USB, no screen, and no reflash. Work runs on KernelBG (never the boot
//! path), so it can't brick boot.
//!
//! Output is delivered two ways:
//!  - On endpoint 0x4000 (works for a direct connection / QEMU).
//!  - As app-log messages on endpoint 2006. The CloudPebble relay (the phone
//!    app's libpebble3) DROPS inbound watch messages whose endpoint it doesn't
//!    know, so our custom 0x4000 reply never reaches a relay-connected tool.
//!    2006 (app logs) IS a known endpoint, so it relays — read it with the
//!    AppLog endpoint / `pebble logs`.

#include "applib/app_logging.h"
#include "console/prompt.h"
#include "pbl/services/comm_session/session.h"
#include "pbl/services/system_task.h"
#include "system/logging.h"

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#define DEV_CONSOLE_ENDPOINT 0x4000
#define APP_LOG_ENDPOINT 2006
#define DEV_CONSOLE_RESP_MAX 1024
#define APPLOG_CHUNK 80  // fits LOG_BUFFER_LENGTH (128) after uuid + log header

static CommSession *s_session;
static char s_cmd[PROMPT_BUFFER_SIZE_BYTES];
static size_t s_cmd_len;
static char s_resp[DEV_CONSOLE_RESP_MAX];
static size_t s_resp_len;
static PromptContext s_ctx;

static void prv_ship_applog(const char *str);

//! Each prompt response line is shipped IMMEDIATELY (app-log) as it is emitted, so a
//! command that hard-faults mid-run -- e.g. a PSRAM bring-up that faults on a bad data
//! path and reboots the watch -- still delivers the lines it printed before the crash
//! (the whole partition-the-failure diagnostic we need). We also accumulate the lines
//! for the 0x4000 direct/QEMU reply sent on completion.
static void prv_response_cb(const char *response) {
  prv_ship_applog(response);
  size_t n = strlen(response);
  if (s_resp_len + n + 1 < DEV_CONSOLE_RESP_MAX) {
    memcpy(s_resp + s_resp_len, response, n);
    s_resp_len += n;
    s_resp[s_resp_len++] = '\n';
  }
}

static int prv_binfmt(char *buf, int len, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = pbl_log_binary_format(buf, len, LOG_LEVEL_INFO, "devcon", 0, fmt, args);
  va_end(args);
  return n;
}

//! Ship one string as an app-log message on endpoint 2006 (a known endpoint the
//! relay forwards). Sent directly (bypasses the app-logging-mode gate).
static void prv_ship_applog(const char *str) {
  CommSession *session = comm_session_get_system_session();
  if (!session) {
    return;
  }
  char log_buffer[LOG_BUFFER_LENGTH];
  AppLogBinaryMessage *msg = (AppLogBinaryMessage *)log_buffer;
  memset(&msg->uuid, 0, sizeof(msg->uuid));
  const size_t off = offsetof(AppLogBinaryMessage, log_msg);
  int n = prv_binfmt((char *)&msg->log_msg, (int)(LOG_BUFFER_LENGTH - off), "%s", str);
  if (n > 0) {
    comm_session_send_data(session, APP_LOG_ENDPOINT, (uint8_t *)log_buffer,
                           off + (size_t)n, COMM_SESSION_DEFAULT_TIMEOUT);
  }
}

//! Fires when the command completes (sync or async) — deliver the output.
static void prv_complete_cb(void) {
  if (!s_session) {
    return;
  }
  if (s_resp_len == 0) {
    static const char kOk[] = "(ok)\n";
    prv_ship_applog("(ok)");  // per-line path shipped nothing; ship the ack
    memcpy(s_resp, kOk, sizeof(kOk) - 1);
    s_resp_len = sizeof(kOk) - 1;
  }
  // 0x4000 reply (direct/QEMU). The app-log copy was already shipped per-line in
  // prv_response_cb as each line was emitted, so partial output survives a crash and
  // there is no need to re-ship the whole buffer here.
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
