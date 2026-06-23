/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! PSRAM bring-up for obelix (SF32LB52J: 16 MB Winbond HYPERBUS PSRAM on MPI1,
//! mapped at 0x60000000 SBUS / 0x10000000 CBUS). init.c parks PSRAM at boot
//! (sets PAD_SA00-SA12 analog; LDO18 is left off because the PSRAM is powered by
//! VDD_SiP, not the internal 1.8 V LDO). This un-parks the pins and brings up the
//! MPI1 controller so it can back the Dart/WAMR pool, driven by the `psram`
//! console command (not auto-run at boot).
//!
//! Mirrors the SiFli SF32LB52 reference exactly (OpenSiFli/SiFli-SDK
//! customer/boards/.../bsp_init.c board_init_psram + common/flash.c
//! bsp_psramc_init): clock MPI1 (FLASH1) off DLL2 @ 288 MHz / div 2 = 144 MHz,
//! then HAL_MPI_PSRAM_Init(handle, cfg, div) with wakeup=0. No manual DQS /
//! calibration -- the HAL handles HBPSRAM internally; the earlier DQS sweep was a
//! dead end. KEY: do NOT enable LDO18 (it fights the external VDD_SiP rail and
//! corrupts reads), and bring the controller up exactly ONCE (re-selecting the
//! DLL2 clock while MPI1 is live HANGS/resets the watch).

#include <bf0_hal.h>

#include "console/prompt.h"
#include "system/logging.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PSRAM_TEST_BASE
#define PSRAM_TEST_BASE 0x60000000u  // MPI1 SBUS alias (CBUS 0x10000000 + 0x50000000)
#endif
#define PSRAM_MSIZE_MB 16            // SF32LB52J embedded PSRAM

static FLASH_HandleTypeDef s_psram_handle;
static bool s_psram_ready;
static bool s_psram_inited;                          // controller brought up (re-init hangs)
static HAL_StatusTypeDef s_psram_init_res = HAL_ERROR;
static uint32_t s_psram_pid = 0xff;

//! Restore the PSRAM pin mux (undo init.c's HAL_PIN_Set_Analog parking).
//! EXACT Winbond HYPERBUS mapping, per SiFli board_pinmux_psram_func1_2_4 case 4
//! ("Winbond 32/64/128p"): 8 data + CLK + CS + DQSDM(NOPULL). Critically, the
//! Winbond part does NOT use DM (SA00) or CLKB (SA06) -- the reference leaves them
//! ANALOG and explicitly comments CLKB out. Driving them (as the OPI variant does)
//! corrupts the data path -> reads fail at word 0.
static void prv_restore_pinmux(void) {
  HAL_PIN_Set(PAD_SA01, MPI1_DIO0, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA02, MPI1_DIO1, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA03, MPI1_DIO2, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA04, MPI1_DIO3, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA08, MPI1_DIO4, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA09, MPI1_DIO5, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA10, MPI1_DIO6, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA11, MPI1_DIO7, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA07, MPI1_CLK, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_SA05, MPI1_CS, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_NOPULL, 1);
  HAL_PIN_Set_Analog(PAD_SA00, 1);  // DM unused on Winbond HYPERBUS
  HAL_PIN_Set_Analog(PAD_SA06, 1);  // CLKB unused on Winbond HYPERBUS
}

typedef void (*PsramEmitFn)(const char *line);

//! Bring up the MPI1 PSRAM controller exactly ONCE. Idempotent: later calls return
//! the cached result without touching the clock/controller (re-init hangs the watch).
//! div = MPI1 clock divider (canonical = 2 -> DLL2 288 MHz / 2 = 144 MHz).
//! Emits a step marker BEFORE each HAL call so that, with per-line console shipping,
//! the LAST line received pinpoints exactly which step hangs/faults (the bring-up has
//! been crashing/hanging the watch; we need to know where).
static HAL_StatusTypeDef prv_psram_init(uint16_t div, PsramEmitFn emit) {
  if (s_psram_inited) {
    return s_psram_init_res;
  }

  // PSRAM is powered by VDD_SiP, not the internal 1.8 V LDO18 (init.c disables LDO18
  // for exactly this reason). Do NOT enable LDO18 -- it fights the external SiP rail
  // and corrupts reads. Just un-park the pins and clock the controller.
  emit("psram step: restore pinmux");
  prv_restore_pinmux();

  // MPI1 (FLASH1) off DLL2 @ 288 MHz; div 2 -> 144 MHz, per the SiFli board_init_psram
  // path. Reference enables DLL2 at 240 THEN 288 (two-step lock) -- replicate it; a
  // single EnableDLL2(288) may not lock. Done once -- re-selecting the clock hangs.
  emit("psram step: EnableDLL2(240)");
  HAL_RCC_HCPU_EnableDLL2(240000000);
  emit("psram step: EnableDLL2(288)");
  HAL_RCC_HCPU_EnableDLL2(288000000);
  emit("psram step: ClockSelect FLASH1<-DLL2");
  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_DLL2);

  // Feed the KernelBG watchdog: controller init can be slow.
  prompt_watchdog_feed();

  emit("psram step: read PID");
  s_psram_pid = (hwp_hpsys_cfg->IDR & HPSYS_CFG_IDR_PID_Msk) >> HPSYS_CFG_IDR_PID_Pos;
  s_psram_pid &= 7;

  qspi_configure_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.Instance = hwp_qspi1;     // MPI1
  cfg.msize = PSRAM_MSIZE_MB;
  cfg.base = QSPI1_MEM_BASE;    // 0x10000000 (CBUS)
  switch (s_psram_pid) {
    case 5: cfg.SpiMode = SPI_MODE_PSRAM; break;     // 16Mb APM QSPI
    case 4: cfg.SpiMode = SPI_MODE_LEGPSRAM; break;  // 32Mb LEGACY
    case 6: cfg.SpiMode = SPI_MODE_HBPSRAM; break;   // Winbond HYPERBUS (obelix)
    case 2:                                          // XCELLA OPI
    case 3:
    default: cfg.SpiMode = SPI_MODE_OPSRAM; break;
  }

  // memset zeroes handle.wakeup = 0 (normal, non-standby boot), per the canonical init.
  memset(&s_psram_handle, 0, sizeof(s_psram_handle));
  emit("psram step: HAL_MPI_PSRAM_Init");
  s_psram_init_res = HAL_MPI_PSRAM_Init(&s_psram_handle, &cfg, div);
  s_psram_inited = true;
  emit("psram step: init returned");
  return s_psram_init_res;
}

//! Write/read sanity test over the PSRAM window. Returns -1 + the failing index,
//! else the number of words verified. (Reads always complete once the controller is
//! up -- a wrong config returns garbage, it does not fault -- so this can't crash.)
static int prv_psram_test(int words, uint32_t *fail_idx) {
  volatile uint32_t *p = (volatile uint32_t *)PSRAM_TEST_BASE;
  for (int i = 0; i < words; i++) {
    p[i] = 0xA5A50000u + (uint32_t)i;
  }
  __DSB();
  for (int i = 0; i < words; i++) {
    if (p[i] != 0xA5A50000u + (uint32_t)i) {
      if (fail_idx) {
        *fail_idx = (uint32_t)i;
      }
      return -1;
    }
  }
  return words;
}

bool sf32lb52_psram_is_ready(void) { return s_psram_ready; }

//! Sink for one diagnostic line (console `psram` command). PsramEmitFn is declared
//! above prv_psram_init so the bring-up can emit per-step markers via the same sink.
static void prv_emit_console(const char *line) { prompt_send_response(line); }

//! Bring the controller up ONCE (idempotent + crash-safe to re-run; re-init would
//! hang), run the partition-the-failure diagnostic + a write/read test, and emit each
//! line via `emit`. Sets s_psram_ready on a passing test. The HBPSRAM path auto-
//! calibrates the SCK/DQS read tap from HCLK/DVFS state, so we read back chip ID + CR0
//! + the ACTUAL calibrated clock to tell power/mode/timing/pinmux apart:
//!   garbage id -> chip not talking (power/reset/mode); good id, bad cr0 -> HYPERBUS
//!   latency not applied; qclk/2 != 144M -> wrong calibration bucket. The w0 read-back
//!   uses distinct per-word values: word0==word1's value -> latency shift; a
//!   bit-permutation -> pinmux/data-lane order; unrelated noise -> power/mode.
static void prv_psram_diag(uint16_t div, PsramEmitFn emit) {
  char buf[160];

  HAL_StatusTypeDef res = prv_psram_init(div, emit);
  sniprintf(buf, sizeof(buf), "psram init div=%u pid=%u -> %s", (unsigned)div,
            (unsigned)s_psram_pid, res == HAL_OK ? "HAL_OK" : "HAL_ERR");
  emit(buf);
  sniprintf(buf, sizeof(buf), "psram clk: dll2=%uHz flash1_src=%d",
            (unsigned)HAL_RCC_HCPU_GetDLL2Freq(),
            (int)HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_FLASH1));
  emit(buf);
  if (res != HAL_OK) {
    return;
  }

  // The register reads (ReadID/ReadCR) spin on TCF with NO timeout and HANG the watch
  // (the read strobe never completes). The MEMORY-MAPPED SBUS read is bounded by the
  // MPI WDTR (armed in init), so it returns garbage instead of hanging -- do it FIRST
  // to capture the read-back PATTERN (the partition-the-failure data) before the
  // hang-prone register reads. Per-line shipping delivers these even if the watch later
  // wedges on ReadID.
  //  word0 reads word1's value -> read latency shift; a bit-permutation of what we wrote
  //  -> pinmux/data-lane order; unrelated noise -> power/mode/strobe.
  volatile uint32_t *p0 = (volatile uint32_t *)PSRAM_TEST_BASE;  // SBUS (WDTR-bounded)
  p0[0] = 0xA5A50000u;
  p0[1] = 0x0000A5A5u;
  __DSB();
  emit("psram step: SBUS read-back");
  sniprintf(buf, sizeof(buf), "psram w0: wrote A5A50000,0000A5A5 read %08x,%08x",
            (unsigned)p0[0], (unsigned)p0[1]);
  emit(buf);

  emit("psram step: SBUS 256-word test");
  uint32_t fail = 0;
  int n = prv_psram_test(256, &fail);
  if (n < 0) {
    sniprintf(buf, sizeof(buf), "psram test FAIL at word %u", (unsigned)fail);
  } else {
    s_psram_ready = true;
    sniprintf(buf, sizeof(buf), "psram test OK (%d words @0x%08x) - %u MB ready",
              n, (unsigned)PSRAM_TEST_BASE, PSRAM_MSIZE_MB);
  }
  emit(buf);

  // Register reads LAST -- these can hang (unbounded TCF spin). We already have the
  // SBUS pattern above. Marked so we know if they're the wedge point.
  emit("psram step: HAL_HYPER_PSRAM_ReadID (may hang)");
  uint16_t id = HAL_HYPER_PSRAM_ReadID(&s_psram_handle, 0);
  emit("psram step: HAL_HYPER_PSRAM_ReadCR");
  uint16_t cr0 = HAL_HYPER_PSRAM_ReadCR(&s_psram_handle, 0);
  emit("psram step: HAL_QSPI_GET_CLK");
  uint32_t qclk = HAL_QSPI_GET_CLK(&s_psram_handle);
  sniprintf(buf, sizeof(buf),
            "psram diag: id=0x%04x cr0=0x%04x qclk=%u/2=%u hclk=%u dvfs=%d",
            (unsigned)id, (unsigned)cr0, (unsigned)qclk, (unsigned)(qclk / 2u),
            (unsigned)HAL_RCC_GetHCLKFreq(CORE_ID_HCPU),
            (int)HAL_RCC_HCPU_GetCurrentDvfsMode());
  emit(buf);
}

//! Console command: `psram [div]` (default div=2). Output goes to the console.
void command_psram(const char *div_str) {
  uint16_t div = 2;
  if (div_str && div_str[0]) {
    int v = atoi(div_str);
    if (v >= 1 && v <= 16) {
      div = (uint16_t)v;
    }
  }
  prv_psram_diag(div, prv_emit_console);
}
