/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! PSRAM bring-up for obelix (SF32LB52J: 16 MB Winbond HYPERBUS PSRAM on MPI1,
//! mapped at 0x60000000 SBUS / 0x10000000 CBUS). init.c parks PSRAM at boot
//! (sets PAD_SA00-SA12 analog; LDO18 is left off because the PSRAM is powered by
//! VDD_SiP, not the internal 1.8 V LDO). This un-parks the pins and brings up the
//! MPI1 controller so it can back the Dart/WAMR pool, driven by the `psram`
//! console command (not auto-run at boot).
//!
//! Mirrors the SiFli SF32LB52 reference (OpenSiFli/SiFli-SDK
//! customer/boards/.../bsp_init.c board_init_psram + common/flash.c
//! bsp_psramc_init): clock MPI1 (FLASH1) off DLL2 @ 288 MHz / div 2 = 144 MHz,
//! then HAL_MPI_PSRAM_Init(handle, cfg, div) with wakeup=0.
//!
//! READ-STROBE FIX (2026-06-23): HAL_MPI_PSRAM_Init's internal auto-calibration
//! (HYPER->OPI->HAL_MPI_OPSRAM_CAL_DELAY) leaves an OFF-CENTER SCK/DQS read tap, so
//! the first real read hangs (the controller spins on the transfer-complete flag with
//! no timeout). Web + HAL research (HyperRAM DQS tuning is the classic failure) says
//! the fix is an EXHAUSTIVE tap sweep -> use the CENTER of the widest passing window.
//! prv_psram_tap_sweep() does exactly that, AFTER init, using the WDTR-bounded SBUS
//! reads (which return garbage instead of hanging) -- unlike the earlier abandoned
//! sweep, which used the UNBOUNDED register reads and wedged (so it looked like a dead
//! end). We also bounded the HAL's CAL_DELAY spin (bf0_hal_mpi_psram.c) so init itself
//! can no longer wedge KernelBG.
//!
//! KEY: do NOT enable LDO18 (it fights the external VDD_SiP rail and corrupts reads),
//! and bring the controller up exactly ONCE (re-selecting the DLL2 clock while MPI1 is
//! live HANGS/resets the watch).

#include <bf0_hal.h>

#include "console/prompt.h"
#include "system/logging.h"

#include "psram.h"
#include "psram_tapwindow.h"

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

  // CLOCK (corrected by on-hardware read-back, 2026-06-24): DLL2 is OFF at boot, and OUR
  // firmware XIPs code from FLASH2/MPI2 clocked by SYS (<-DLL1), NOT DLL2 (the SiFli reference
  // points XIP at DLL2; we don't). So enabling DLL2 here does NOT touch the code-fetch clock
  // -- the earlier "EnableDLL2 freezes XIP" theory is WRONG. We MUST enable DLL2 (it's off)
  // to clock the PSRAM (FLASH1/MPI1) at 288MHz; two-step 240->288 per the reference (a single
  // 288 may not lock). The RCC DLL2-READY spin is bounded by our HAL patch so EnableDLL2
  // cannot wedge. Fine markers + a ~20ms flush delay around each call so the LAST shipped
  // marker pins exactly which call stalls (the bring-up has hung somewhere around here).
  emit("psram step: EnableDLL2(240) call"); HAL_Delay_us(20000);
  HAL_RCC_HCPU_EnableDLL2(240000000);
  emit("psram step: EnableDLL2(240) ret"); HAL_Delay_us(20000);
  emit("psram step: EnableDLL2(288) call"); HAL_Delay_us(20000);
  HAL_RCC_HCPU_EnableDLL2(288000000);
  emit("psram step: EnableDLL2(288) ret"); HAL_Delay_us(20000);
  {
    char clkbuf[128];
    uint32_t dll2_hz = HAL_RCC_HCPU_GetDLL2Freq();
    sniprintf(clkbuf, sizeof(clkbuf), "psram clk: dll2=%uHz flash2_src=%d (XIP on SYS<-DLL1)",
              (unsigned)dll2_hz, (int)HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_FLASH2));
    emit(clkbuf); HAL_Delay_us(20000);
    if (dll2_hz == 0) {
      emit("psram ERROR: DLL2 didn't enable -- aborting"); return HAL_ERROR;
    }
  }
  emit("psram step: ClockSelect FLASH1<-DLL2 call"); HAL_Delay_us(20000);
  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_DLL2);
  emit("psram step: ClockSelect ret"); HAL_Delay_us(20000);

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
  // ROW-BOUNDARY FIX. HAL_MPI_PSRAM_Init inherits RBSIZE=7 (=2^(7+3)=1024B, the 3-bit
  // field's max) from the OPI/XCCELA bring-up path -- it is never chosen for the Winbond
  // HyperBus part. With a 1KB row boundary the controller chops linear bursts at 1KB and
  // mis-addresses across the real (larger) device row, so anything spanning >1KB corrupts
  // (matches the observed 1KB usable cliff exactly). Override to 0 = no row boundary so
  // linear AHB bursts run uninterrupted. (DCR.RBSIZE is a single MODIFY_REG; safe post-init.)
  if (s_psram_init_res == HAL_OK && cfg.SpiMode == SPI_MODE_HBPSRAM) {
    HAL_FLASH_SET_ROW_BOUNDARY(&s_psram_handle, 0u);
    emit("psram step: RBSIZE override -> 0 (no row boundary)");
  }
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

//! Exhaustive read-strobe tap sweep. The HAL auto-cal leaves an off-center SCK/DQS tap
//! (so reads hang/corrupt); re-derive the right one empirically. For each SCK candidate,
//! sweep the full DQS range writing a TAP-UNIQUE marker and reading it back (the SBUS
//! path is WDTR-bounded -- a bad tap returns garbage, it never hangs), build a pass map,
//! and pick the CENTER of the widest passing window (psram_pick_tap_center). Leaves the
//! best (SCK,DQS) applied. Returns the chosen DQS tap, or -1 if nothing reads back.
//! Tap-unique markers prevent a stale-latch false pass (a bad read returning the prior
//! tap's data). Must run AFTER HAL_MPI_PSRAM_Init (it arms the WDTR + leaves the
//! controller up); feeds the KernelBG watchdog across the sweep.
#define PSRAM_TAP_MAX 256
static int prv_psram_tap_sweep(PsramEmitFn emit) {
  char buf[160];
  volatile uint32_t *p = (volatile uint32_t *)PSRAM_TEST_BASE;
  static unsigned char pass[PSRAM_TAP_MAX];
  // SCK candidates: low delays cover the usable range at 144MHz; the auto-cal's own
  // value is typically small. (DQS is the dominant read-strobe knob; SCK is the outer
  // loop.) 0 last so a nonzero delay is preferred on a width tie.
  static const uint8_t sck_cands[] = {1, 2, 3, 4, 5, 6, 8, 0};
  int best_sck = -1, best_lo = -1, best_hi = -1, best_center = -1, best_w = -1;

  for (unsigned si = 0; si < sizeof(sck_cands); si++) {
    uint8_t sck = sck_cands[si];
    HAL_MPI_SET_SCK(&s_psram_handle, sck, 0);
    for (int dqs = 0; dqs < PSRAM_TAP_MAX; dqs++) {
      HAL_MPI_SET_DQS_DELAY(&s_psram_handle, (uint8_t)dqs);
      HAL_Delay_us(10);  // strobe settle (HAL uses 50us after a final apply)
      __DSB();
      uint32_t marker = 0xC0DE0000u ^ ((uint32_t)sck << 12) ^ ((uint32_t)dqs << 4);
      bool ok = true;
      for (int i = 0; i < 8; i++) {
        p[i] = marker + (uint32_t)i;
      }
      __DSB();
      for (int i = 0; i < 8 && ok; i++) {
        ok = (p[i] == marker + (uint32_t)i);
      }
      pass[dqs] = ok ? 1u : 0u;
      if ((dqs & 0x1f) == 0) {
        prompt_watchdog_feed();
      }
    }
    int lo = -1, hi = -1;
    int center = psram_pick_tap_center(pass, PSRAM_TAP_MAX, &lo, &hi);
    int width = (center < 0) ? -1 : (hi - lo);
    sniprintf(buf, sizeof(buf), "psram sweep: sck=%u dqs window=[%d..%d] w=%d",
              (unsigned)sck, lo, hi, width);
    emit(buf);
    if (center >= 0 && width > best_w) {
      best_w = width;
      best_sck = sck;
      best_lo = lo;
      best_hi = hi;
      best_center = center;
    }
    prompt_watchdog_feed();
  }

  if (best_center < 0) {
    emit("psram sweep: NO passing tap at any sck/dqs (not a tap problem -> power/pinmux/latency)");
    return -1;
  }
  HAL_MPI_SET_SCK(&s_psram_handle, (uint8_t)best_sck, 0);
  HAL_MPI_SET_DQS_DELAY(&s_psram_handle, (uint8_t)best_center);
  HAL_Delay_us(50);
  __DSB();
  sniprintf(buf, sizeof(buf), "psram sweep: CHOSE sck=%d dqs=%d window=[%d..%d] w=%d",
            best_sck, best_center, best_lo, best_hi, best_w);
  emit(buf);
  return best_center;
}

bool sf32lb52_psram_is_ready(void) { return s_psram_ready; }

//! USABLE-SIZE PROBE (non-caching). A single write-one/read-one passes anywhere and a 1KB
//! sequential test passes, but writing MANY cells across a large span then reading them back
//! fails (and the EMS heap corrupts) -- it is NOT clean power-of-two aliasing. So measure what
//! actually works the way a heap uses it: the largest contiguous region that survives a full
//! write-all-then-read-all. Test doubling sizes from 1KB; return the largest that verifies
//! exactly (the first FAILURE pinpoints the corruption cliff). SBUS reads/writes are WDTR-
//! bounded (can't hang); feed the watchdog through the long scans.
static uint32_t prv_psram_usable_bytes(void) {
  volatile uint32_t *base = (volatile uint32_t *)PSRAM_TEST_BASE;
  uint32_t usable = 0u;
  for (uint32_t sz = 1024u; sz <= SF32LB52_PSRAM_SIZE; sz <<= 1) {
    const uint32_t words = sz / 4u;
    for (uint32_t i = 0; i < words; i++) {
      base[i] = 0x5A5A0000u ^ i;
      if ((i & 0x3fffu) == 0u) {
        prompt_watchdog_feed();
      }
    }
    __DSB();
    bool ok = true;
    for (uint32_t i = 0; i < words; i++) {
      if (base[i] != (0x5A5A0000u ^ i)) {
        ok = false;
        break;
      }
      if ((i & 0x3fffu) == 0u) {
        prompt_watchdog_feed();
      }
    }
    if (!ok) {
      break;
    }
    usable = sz;  // this size fully verified; try the next
  }
  return usable;
}

uint32_t sf32lb52_psram_size(void) {
  static uint32_t s_probed_size;  // cached; 0 = not yet probed (or unusable)
  if (!s_psram_ready) {
    return 0u;
  }
  if (s_probed_size == 0u) {
    s_probed_size = prv_psram_usable_bytes();
  }
  return s_probed_size;
}

//! Sink for one diagnostic line (console `psram` command). PsramEmitFn is declared
//! above prv_psram_init so the bring-up can emit per-step markers via the same sink.
static void prv_emit_console(const char *line) { prompt_send_response(line); }

//! Bring the controller up ONCE (idempotent + crash-safe to re-run; re-init would
//! hang), run the partition-the-failure diagnostic + a write/read test, and emit each
//! line via `emit`. Sets s_psram_ready on a passing test. The HBPSRAM path auto-
//! calibrates the SCK/DQS read tap from HCLK/DVFS state; when that tap is wrong the
//! reads come back corrupted. We DON'T touch the chip's HYPERBUS register reads here
//! (they hang -- see below); the WDTR-bounded SBUS w0 read-back uses distinct per-word
//! values to tell the failure modes apart:
//!   word0 == word1's value -> read latency shift; a bit-permutation of what we wrote
//!   -> pinmux/data-lane order; unrelated noise -> power/mode/strobe; correct -> ready.
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

  // THE FIX: re-derive the read strobe. The HAL auto-cal left an off-center SCK/DQS tap;
  // sweep for the center of the widest passing window and apply it (WDTR-bounded reads,
  // can't hang). After this the SBUS read-back below should be correct.
  emit("psram step: tap sweep");
  prv_psram_tap_sweep(emit);

  // SBUS read-back PATTERN (post-sweep). MEMORY-MAPPED SBUS read is bounded by the MPI
  // WDTR (armed in init), so it returns garbage instead of hanging.
  //  word0 reads word1's value -> read latency shift; a bit-permutation of what we wrote
  //  -> pinmux/data-lane order; unrelated noise -> power/mode/strobe; correct -> fixed.
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

  // REAL SIZE: the 256-word test only proves the first 1KB. The die may be smaller than the
  // configured window and ALIAS (high addresses wrap onto low) -- which corrupts any heap
  // placed across the full window. Probe the true size by finding the alias-wrap boundary.
  if (s_psram_ready) {
    uint32_t usable = sf32lb52_psram_size();  // largest write-all/read-all-reliable region
    sniprintf(buf, sizeof(buf), "psram USABLE: %u KB (%u MB) reliable of %u MB window",
              (unsigned)(usable / 1024u), (unsigned)(usable / (1024u * 1024u)), PSRAM_MSIZE_MB);
    emit(buf);

    // RBSIZE SWEEP (diagnostic). RBSIZE=7(1KB) and 0(none) both yielded ~4KB usable, so sweep
    // the whole 3-bit field LIVE -- HAL_FLASH_SET_ROW_BOUNDARY is a single MODIFY_REG (no
    // re-init) -- and report usable per value. Either a value unlocks MB-scale (the fix) or all
    // stay ~4KB (RBSIZE ruled out -> the cliff is elsewhere, e.g. CR0 wrapped-burst). bytes per
    // rb: 0=none, n>=1 => 2^(n+3) (rb=4=>128B .. rb=7=>1KB).
    uint32_t best_rb = 0u, best_usable = 0u;
    for (uint32_t rb = 0u; rb <= 7u; rb++) {
      HAL_FLASH_SET_ROW_BOUNDARY(&s_psram_handle, (uint8_t)rb);
      uint32_t u = prv_psram_usable_bytes();
      sniprintf(buf, sizeof(buf), "psram rbsweep: rb=%u usable=%u KB", (unsigned)rb,
                (unsigned)(u / 1024u));
      emit(buf);
      if (u > best_usable) {
        best_usable = u;
        best_rb = rb;
      }
    }
    HAL_FLASH_SET_ROW_BOUNDARY(&s_psram_handle, (uint8_t)best_rb);  // leave controller at best
    sniprintf(buf, sizeof(buf), "psram rbsweep: BEST rb=%u -> %u KB", (unsigned)best_rb,
              (unsigned)(best_usable / 1024u));
    emit(buf);
  }

  // NOTE: the HYPERBUS register reads (HAL_HYPER_PSRAM_ReadID/ReadCR) are deliberately
  // NOT called here -- they spin on TCF with no timeout and HANG KernelBG forever (the
  // read strobe never completes), wedging the console and forcing a reboot every run.
  // The SBUS read-back + test above is WDTR-bounded (returns garbage, never hangs) and
  // gives us the partition-the-failure pattern. Restore the register reads only once the
  // read path is fixed (bounded reads + a working SCK/DQS tap).
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
