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
  HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_NOPULL, 1);  // Winbond case uses NOPULL (board_psram.c func4 :96); a pulldown biases the DQS strobe -> cal can't lock
  HAL_PIN_Set_Analog(PAD_SA00, 1);  // DM unused on Winbond HYPERBUS
  HAL_PIN_Set_Analog(PAD_SA06, 1);  // CLKB unused on Winbond HYPERBUS
}

typedef void (*PsramEmitFn)(const char *line);

// Defined later; forward-declared so prv_psram_init can ship a bulk-usable number EARLY (before the
// long console diag drops the BLE session).
static uint32_t prv_psram_usable_at(uint32_t base_addr, uint32_t max_sz);

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

  // POWER (corrected 2026-06-25 via SiFli HW spec + SDK): on the SF32LB52J SiP the PSRAM die is
  // fed by the INTERNAL 1.8V LDO -- LDO18 / PMU_PERI_LDO_1V8 == VDD18_VOUT, listed as "SiP power".
  // There is NO separate external VDD_SiP rail on the J part. init.c powers LDO18 DOWN at boot on
  // a (wrong) "VDD_SiP powers it" theory, which leaves the PSRAM UNPOWERED -- so the read-strobe
  // auto-cal can't sample the device (CALCR.DONE never asserts) and every bulk read corrupts
  // ("no passing DQS tap anywhere" = power, not tap-centering). Enable LDO18 before bring-up and
  // let the rail settle. (The SDK reference sf32lb52-lcd_base enables LDO18 for this HyperBus part.)
  // Enable LDO18 by directly UNDOING init.c's power-down: clear LDO18_PD (init.c SET it -- and
  // PD overrides EN, so HAL_PMU_ConfigPeriLdo(en=true) alone may leave the rail off) AND set
  // EN_LDO18. Report PERI_LDO so we can confirm the rail is actually on (EN=1, PD=0).
  emit("psram step: enable LDO18 (clear PD + set EN)");
  hwp_pmuc->PERI_LDO &= ~PMUC_PERI_LDO_LDO18_PD_Msk;
  hwp_pmuc->PERI_LDO |= PMUC_PERI_LDO_EN_LDO18_Msk;
  HAL_Delay_us(5000);
  {
    char lb[72];
    sniprintf(lb, sizeof(lb), "psram LDO18: PERI_LDO=0x%08x", (unsigned)hwp_pmuc->PERI_LDO);
    emit(lb);
  }

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
  // CLOCK EXPERIMENT (-80): DLL2 @144MHz -> HyperBus 72MHz (MPI1 div 2). At 144MHz the device read
  // came back ZERO at every DQS tap (w0=0), which points to a WHOLE-CYCLE read-latency error (the
  // HAL's 144MHz CR0 latency code likely under-counts; datasheet says 144MHz needs the 6-clock
  // setting). No sub-cycle tap can fix a whole-cycle miss. At 72MHz, HAL_HYPER_PSRAM_Init auto-picks
  // the unambiguous low-clock latency code (freq<=85MHz branch) and the data-valid window is far
  // wider -> real reads should land. If w0/USABLE come back correct here, the wall was read latency,
  // not silicon. Two-step ramp 120->144 (a single high step may not lock; 144 is well within range).
  emit("psram step: EnableDLL2(120) call"); HAL_Delay_us(20000);
  HAL_RCC_HCPU_EnableDLL2(120000000);
  emit("psram step: EnableDLL2(120) ret"); HAL_Delay_us(20000);
  emit("psram step: EnableDLL2(144) call"); HAL_Delay_us(20000);
  HAL_RCC_HCPU_EnableDLL2(144000000);
  emit("psram step: EnableDLL2(144) ret"); HAL_Delay_us(20000);
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
    // PID=6 = BOOT_PSRAM_WINBOND (HyperBus) per the chip's own IDR + the SiFli reference mapping.
    // (-76 tried SPI_MODE_OPSRAM in case the part were actually OPI-PSRAM, as some SiFli docs claim
    // for the "J" variant -- but the cal still didn't lock and bulk still corrupted, identical to
    // HyperBus. So OPI is ruled out; PID=6=Winbond stands. The bulk wall is NOT the protocol.)
    case 6: cfg.SpiMode = SPI_MODE_HBPSRAM; break;   // Winbond HYPERBUS (obelix; PID=6)
    case 2:                                          // XCELLA OPI
    case 3:
    default: cfg.SpiMode = SPI_MODE_OPSRAM; break;
  }

  // WAKE the PSRAM from deep-power-down + enable the MPI controller BEFORE init -- exactly as
  // the SDK bootloader does (board_psram.c builds a temp handle on the MPI instance and calls
  // HAL_MPI_EXIT_LOWP before HAL_MPI_PSRAM_Init). Obelix never did this. The consequence (root
  // cause of the bulk-corruption wall): the read-strobe auto-cal (HAL_MPI_OPSRAM_CAL_DELAY, run
  // inside init) drives its own probe reads, but on an unwoken/unenabled chip they return
  // nothing -> CALCR.DONE never asserts -> uncalibrated strobe -> sustained/burst reads corrupt
  // (small reads happen to land in a wide enough window). HAL_PSRAM_RESET is a no-op for HBPSRAM
  // so init never wakes the chip itself. The CS-pulse wake needs ~150us (HBPSRAM branch).
  {
    FLASH_HandleTypeDef wake;
    memset(&wake, 0, sizeof(wake));
    wake.Instance = cfg.Instance;
    HAL_MPI_EXIT_LOWP(&wake, cfg.SpiMode);
    emit("psram step: EXIT_LOWP (wake from deep-power-down)");
  }

  // memset zeroes handle.wakeup = 0 (normal, non-standby boot), per the canonical init.
  memset(&s_psram_handle, 0, sizeof(s_psram_handle));
  emit("psram step: HAL_MPI_PSRAM_Init");
  s_psram_init_res = HAL_MPI_PSRAM_Init(&s_psram_handle, &cfg, div);
  // The HAL runs its read-strobe cal (HAL_MPI_OPSRAM_CAL_DELAY) INSIDE HAL_OPI_PSRAM_Init -- i.e.
  // in OPI framing, BEFORE HAL_HYPER_PSRAM_Init enables HyperBus (HAL_FLASH_ENABLE_HYPER) and writes
  // the CR0 read-latency (verified: HAL_HYPER_PSRAM_Init order = OPI_PSRAM_Init -> ENABLE_HYPER ->
  // WriteCR -> EN_FIXLAT, bf0_hal_mpi_psram.c). A Winbond HyperBus die (PID=6) can't return a valid
  // read strobe in OPI framing, so that first cal's CALCR.DONE never asserts (we saw CALCR=0x0 across
  // -73/-74 with the rail CONFIRMED powered). FIX (research-backed): now that init has enabled
  // HyperBus + written CR0, RE-RUN the cal in the correct framing via HAL_MPI_OPSRAM_AUTO_CAL. (An
  // earlier note here said "do NOT re-run AUTO_CAL" -- that was wrong; the FIRST cal is the
  // mis-sequenced one, re-running in HyperBus framing is the whole point.) Report both so we can see
  // OPI-framing DONE=0 flip to HB-framing DONE=1. The diag's later CALCR read (ships reliably)
  // reflects this post-re-cal state; these in-init emits may batch away.
  if (s_psram_init_res == HAL_OK) {
    char cb[112];
    uint32_t cc = s_psram_handle.Instance->CALCR;
    sniprintf(cb, sizeof(cb), "psram CAL(init,OPI-framing): CALCR=0x%08x DONE=%u DELAY=%u", (unsigned)cc,
              (unsigned)((cc & MPI_CALCR_DONE_Msk) >> MPI_CALCR_DONE_Pos),
              (unsigned)((cc & MPI_CALCR_DELAY_Msk) >> MPI_CALCR_DELAY_Pos));
    emit(cb);
    uint8_t rsck = 0, rdqs = 0;
    int calres = HAL_MPI_OPSRAM_AUTO_CAL(&s_psram_handle, &rsck, &rdqs);
    uint32_t cc2 = s_psram_handle.Instance->CALCR;
    sniprintf(cb, sizeof(cb),
              "psram CAL(recal,HB-framing): res=%d CALCR=0x%08x DONE=%u DELAY=%u sck=%u dqs=%u",
              calres, (unsigned)cc2,
              (unsigned)((cc2 & MPI_CALCR_DONE_Msk) >> MPI_CALCR_DONE_Pos),
              (unsigned)((cc2 & MPI_CALCR_DELAY_Msk) >> MPI_CALCR_DELAY_Pos),
              (unsigned)rsck, (unsigned)rdqs);
    emit(cb);
  }
  // NOTE: earlier builds (-62..-65) overrode RBSIZE/CR0/CSLMAX here to chase a ~1-4KB "usable"
  // cliff. That cliff was the UNCACHED memory-mapped long-burst continuation being broken, NOT a
  // controller-register problem -- the real fix is the cacheable MPU region over 0x60000000
  // (system_bf0_ap.c) so the D-cache batches access into 32B line bursts. Those overrides were
  // treating the symptom on the wrong path, so they're removed: the controller now uses the HAL
  // defaults (what the SiFli SDK runs with). If cached access still misbehaves, re-investigate
  // the cache controller's PSRAM access / the SCK-DQS tap under cached bursts, not these knobs.
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
static uint32_t prv_psram_usable_at(uint32_t base_addr, uint32_t max_sz) {
  volatile uint32_t *base = (volatile uint32_t *)base_addr;
  uint32_t usable = 0u;
  for (uint32_t sz = 8u; sz <= max_sz; sz <<= 1) {  // start at 8B (2 words) to find the exact single->bulk break
    const uint32_t words = sz / 4u;
    for (uint32_t i = 0; i < words; i++) {
      base[i] = 0x5A5A0000u ^ i;
      if ((i & 0x3fffu) == 0u) {
        prompt_watchdog_feed();
      }
    }
    __DSB();
    // The PSRAM window is cacheable (write-back), so flush the just-written lines out to PSRAM
    // and drop the cache -- otherwise the read-back below would hit the cache and pass trivially
    // without ever exercising the silicon. After this, reads miss and re-fetch from PSRAM.
    SCB_CleanInvalidateDCache();
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

static uint32_t prv_psram_usable_bytes(void) {
  return prv_psram_usable_at(PSRAM_TEST_BASE, SF32LB52_PSRAM_SIZE);
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

  // === FAST DIAGNOSTIC (ships before the BLE session drops) ===
  // The full tap sweep + 64KB taptest below run long, and the no-bond session reliably drops mid-
  // way, so CR0/USABLE never make it out. Emit the decisive signals FIRST, then return. We judge by
  // REAL reads, not CALCR.DONE (which never asserts on this part). w0 uses the cal's tap; CR0 should
  // read back HAL_HYPER_PSRAM_Init's mr0 (72MHz->0xe78f); USABLE says whether BULK works (256KB).
  {
    volatile uint32_t *sb = (volatile uint32_t *)PSRAM_TEST_BASE;
    sb[0] = 0xA5A50000u;
    sb[1] = 0x0000A5A5u;
    __DSB();
    sniprintf(buf, sizeof(buf), "psram FAST w0: wrote A5A50000,0000A5A5 read %08x,%08x",
              (unsigned)sb[0], (unsigned)sb[1]);
    emit(buf);
    uint16_t id0 = HAL_HYPER_PSRAM_ReadID(&s_psram_handle, 0);
    uint16_t cr0 = HAL_HYPER_PSRAM_ReadCR(&s_psram_handle, 0);
    uint16_t cr1 = HAL_HYPER_PSRAM_ReadCR(&s_psram_handle, 1);
    sniprintf(buf, sizeof(buf), "psram FAST HB: ID0=0x%04x CR0=0x%04x CR1=0x%04x",
              (unsigned)id0, (unsigned)cr0, (unsigned)cr1);
    emit(buf);
    uint32_t us = prv_psram_usable_at(PSRAM_TEST_BASE, 64u * 1024u);
    sniprintf(buf, sizeof(buf), "psram FAST USABLE (cal tap): SBUS=%uB", (unsigned)us);
    emit(buf);
    // The 8B break could be a bad (uncentered) strobe tap. Sweep SCK x DQS and, at each tap, measure
    // the largest sustained round-trip. If SOME tap sustains a long burst -> it's the tap (use it);
    // if EVERY tap caps near 8B -> the memory-mapped burst itself is limited (need DMA).
    uint8_t best_sck = 0u, best_dqs = 0u;
    uint32_t best_break = 0u;
    static const uint8_t scks[] = {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u};
    for (unsigned si = 0; si < sizeof(scks) / sizeof(scks[0]); si++) {
      HAL_MPI_SET_SCK(&s_psram_handle, scks[si], 0);
      for (uint32_t d = 0u; d <= 224u; d += 32u) {
        HAL_MPI_SET_DQS_DELAY(&s_psram_handle, (uint8_t)d);
        uint32_t b = prv_psram_usable_at(PSRAM_TEST_BASE, 64u * 1024u);
        if (b > best_break) {
          best_break = b;
          best_sck = scks[si];
          best_dqs = (uint8_t)d;
        }
      }
      prompt_watchdog_feed();
    }
    sniprintf(buf, sizeof(buf), "psram TAP-BREAK best: sck=%u dqs=%u break=%uB",
              (unsigned)best_sck, (unsigned)best_dqs, (unsigned)best_break);
    emit(buf);
    HAL_MPI_SET_SCK(&s_psram_handle, best_sck, 0);
    HAL_MPI_SET_DQS_DELAY(&s_psram_handle, best_dqs);
    s_psram_ready = (best_break > 0u);
    return;  // skip the old long tap sweep + 64KB taptest below
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

  // DECISIVE DIAGNOSTIC: read the HyperBus device ID + CR0 back. SAFE now -- HAL_FLASH_READ32 is a
  // bare DR read (no spin) and HAL_FLASH_SET_CMD's TCF spin is bounded by our patch (the old
  // "ReadID hangs" was the PRE-patch unbounded TCF). Placed early so it ships before the long tap
  // sweep. ID0 != 0/0xffff => device answers in HyperBus framing. CR0 should equal HAL_HYPER_PSRAM_
  // Init's mr0 for the current clock (72MHz -> (14<<12)|0x078f = 0xe78f); if CR0 differs, the latency
  // write didn't land (byte-swap/framing) -- which would explain zero reads at every tap.
  if (s_psram_ready) {
    uint16_t hb_id0 = HAL_HYPER_PSRAM_ReadID(&s_psram_handle, 0);
    uint16_t hb_cr0 = HAL_HYPER_PSRAM_ReadCR(&s_psram_handle, 0);
    uint16_t hb_cr1 = HAL_HYPER_PSRAM_ReadCR(&s_psram_handle, 1);
    sniprintf(buf, sizeof(buf), "psram HB readback: ID0=0x%04x CR0=0x%04x CR1=0x%04x",
              (unsigned)hb_id0, (unsigned)hb_cr0, (unsigned)hb_cr1);
    emit(buf);
  }

  // REAL SIZE: the 256-word test only proves the first 1KB. The die may be smaller than the
  // configured window and ALIAS (high addresses wrap onto low) -- which corrupts any heap
  // placed across the full window. Probe the true size by finding the alias-wrap boundary.
  if (s_psram_ready) {
    uint32_t usable = sf32lb52_psram_size();  // largest write-all/read-all-reliable region (SBUS)
    sniprintf(buf, sizeof(buf), "psram USABLE: %u KB (%u MB) reliable of %u MB window (SBUS)",
              (unsigned)(usable / 1024u), (unsigned)(usable / (1024u * 1024u)), PSRAM_MSIZE_MB);
    emit(buf);
    // CBUS (cached port 0x10000000) bulk probe -- the REAL test of cached bulk. The SBUS probe
    // above bypasses the cache (SBUS is the uncached system-bus alias), so it only measured the
    // broken uncached long-burst continuation. The SDK routes cached bulk through CBUS, where the
    // D-cache batches access into 32B cache-line bursts the controller handles. The cacheable MPU
    // region now sits over CBUS (system_bf0_ap.c). Cap at 1MB to bound exposure if a fill wedges.
    uint32_t usable_cbus = prv_psram_usable_at(0x10000000u, 1u * 1024u * 1024u);
    sniprintf(buf, sizeof(buf), "psram USABLE-CBUS: %u KB reliable (cached port 0x10000000)",
              (unsigned)(usable_cbus / 1024u));
    emit(buf);
  }

  // VALIDATE the read-strobe-tap hypothesis instead of assuming it. First report the HARDWARE
  // auto-cal result: CALCR.DONE (did the calibration complete on this unit?) + the chosen DELAY.
  // Then SCORE a spread of DQS taps by a REAL 64KB cached write-all/read-all (the trivial 8-word
  // sweep passes at every tap; sustained bursts are the real test). If some tap PASSES 64KB where
  // others fail -> the tap IS the lever (worth a full fine sweep); if ALL fail -> the tap is NOT
  // the cause (look elsewhere / suspect silicon). Leaves DQS at the best-scoring tap for dart.
  if (s_psram_ready) {
    sniprintf(buf, sizeof(buf), "psram LDO18: PERI_LDO=0x%08x (EN=%u PD=%u)",
              (unsigned)hwp_pmuc->PERI_LDO,
              (unsigned)((hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_EN_LDO18_Msk) ? 1u : 0u),
              (unsigned)((hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_LDO18_PD_Msk) ? 1u : 0u));
    emit(buf);
    uint32_t calcr = s_psram_handle.Instance->CALCR;
    sniprintf(buf, sizeof(buf), "psram CALCR: DONE=%u DELAY=%u EN=%u (raw=0x%08x)",
              (unsigned)((calcr & MPI_CALCR_DONE_Msk) >> MPI_CALCR_DONE_Pos),
              (unsigned)((calcr & MPI_CALCR_DELAY_Msk) >> MPI_CALCR_DELAY_Pos),
              (unsigned)((calcr & MPI_CALCR_EN_Msk) >> MPI_CALCR_EN_Pos), (unsigned)calcr);
    emit(buf);

    // NOTE: do NOT read the HyperBus device ID/CR here -- HAL_HYPER_PSRAM_ReadID HANGS KernelBG on
    // this part (the TCF wait in that path is not bounded by our spin patch). Confirmed on -79: the
    // diag wedged exactly at the readback. Use real array reads (w0 / USABLE) to judge instead.
    volatile uint32_t *base = (volatile uint32_t *)PSRAM_TEST_BASE;
    const uint32_t words = (64u * 1024u) / 4u;
    static const uint8_t dqs_probe[] = {0u,  16u, 32u,  48u,  64u,  80u,  96u,  112u,
                                        128u, 144u, 160u, 176u, 192u, 208u, 224u, 240u};
    uint8_t best_dqs = 0u;
    uint32_t best_ok = 0u;
    bool any_pass = false;
    for (unsigned t = 0; t < sizeof(dqs_probe); t++) {
      HAL_MPI_SET_DQS_DELAY(&s_psram_handle, dqs_probe[t]);
      HAL_Delay_us(50);
      for (uint32_t i = 0; i < words; i++) {
        base[i] = 0x7A7A0000u ^ i;
        if ((i & 0x3fffu) == 0u) {
          prompt_watchdog_feed();
        }
      }
      __DSB();
      SCB_CleanInvalidateDCache();  // force the read-back to re-fetch from PSRAM, not the cache
      uint32_t okw = 0u;
      bool ok = true;
      for (uint32_t i = 0; i < words; i++) {
        if (base[i] != (0x7A7A0000u ^ i)) {
          ok = false;
          break;
        }
        okw++;
        if ((i & 0x3fffu) == 0u) {
          prompt_watchdog_feed();
        }
      }
      if (okw > best_ok) {
        best_ok = okw;
        best_dqs = dqs_probe[t];
      }
      if (ok) {
        any_pass = true;
      }
      sniprintf(buf, sizeof(buf), "psram taptest: dqs=%u %s ok=%u/%u", (unsigned)dqs_probe[t],
                ok ? "PASS" : "fail", (unsigned)okw, (unsigned)words);
      emit(buf);
    }
    HAL_MPI_SET_DQS_DELAY(&s_psram_handle, best_dqs);
    HAL_Delay_us(50);
    sniprintf(buf, sizeof(buf), "psram taptest: BEST dqs=%u ok=%u/%u any_pass=%u",
              (unsigned)best_dqs, (unsigned)best_ok, (unsigned)words, (unsigned)any_pass);
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
