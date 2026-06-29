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
//! READ-STROBE FIX: the per-boot read-strobe correctness is owned by the controller-clock
//! configuration in prv_psram_init (DLL2 ramped to 288MHz so the in-HAL delay-line cal locks),
//! NOT by a post-init strobe sweep. We also bounded the HAL's CAL_DELAY spin (bf0_hal_mpi_psram.c)
//! so init itself can no longer wedge KernelBG.
//!
//! KEY: do NOT enable LDO18 (it fights the external VDD_SiP rail and corrupts reads),
//! and bring the controller up exactly ONCE (re-selecting the DLL2 clock while MPI1 is
//! live HANGS/resets the watch).

#include <bf0_hal.h>

#include "console/prompt.h"
#include "system/logging.h"

#include "psram.h"

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
  // CLOCK FIX (2026-06-28): the vendor (boot_flash.c:386) runs DLL2 @ 288MHz for 52x PSRAM. The
  // HyperBus path forces PSCLR=1 (HAL_OPI_PSRAM_Init ignores the div arg -- "OPI PSRAM do not care"),
  // so the MPI controller = DLL2/1 = 288MHz and the cal clock = 288/2 = 144MHz; HAL_HYPER_PSRAM_Init
  // then auto-selects the matching 144MHz CR0 read-latency code (0x178f). We were at DLL2=144 ->
  // controller 144MHz, cal clock 72MHz, CR0 0xe78f (the <=85MHz code) -- a coherent-but-WRONG pair:
  // the delay-line cal couldn't lock at the half clock (CALCR.DONE=0 -> garbage SCK/DQS -> per-boot
  // variance + marginal reads). Ramp 144->288 (24MHz DLL step; both quantize) so the cal locks.
  emit("psram step: EnableDLL2(144) call"); HAL_Delay_us(20000);
  HAL_RCC_HCPU_EnableDLL2(144000000);
  emit("psram step: EnableDLL2(144) ret"); HAL_Delay_us(20000);
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
    // The HAL ran its read-strobe cal (a pure delay-line lock, HAL_MPI_OPSRAM_CAL_DELAY) INSIDE
    // HAL_OPI_PSRAM_Init at cal clock = controller/2 (it sets PSCLR=2 during the cal). With the
    // controller now at the vendor 288MHz (-> 144MHz cal clock) the delay line should LOCK and
    // CALCR.DONE assert (it read 0 at our old 72MHz cal clock). We do NOT re-run the cal:
    // HAL_MPI_OPSRAM_AUTO_CAL is the SAME routine (not a separate "HB-framing" cal -- that premise
    // was wrong); re-running it post-init only re-applies the same SCK/DQS. Report DONE + the actual
    // controller clock (HAL_QSPI_GET_CLK) so the clock fix is verifiable on-device.
    char cb[128];
    uint32_t cc = s_psram_handle.Instance->CALCR;
    uint32_t ctlr = HAL_QSPI_GET_CLK(&s_psram_handle);
    sniprintf(cb, sizeof(cb), "psram CAL: ctlr=%uHz CALCR=0x%08x DONE=%u DELAY=%u", (unsigned)ctlr,
              (unsigned)cc, (unsigned)((cc & MPI_CALCR_DONE_Msk) >> MPI_CALCR_DONE_Pos),
              (unsigned)((cc & MPI_CALCR_DELAY_Msk) >> MPI_CALCR_DELAY_Pos));
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

// NOTE: an earlier build added prv_psram_set_rxclkinv() to sweep MISCR.RXCLKINV -- removed. On 52x
// the vendor never sets RXCLKINV on the HyperBus path (it stays 0); writing it post-init wedged the
// controller. The real per-boot fix is the controller-clock correction in prv_psram_init (DLL2 288MHz
// so the in-HAL delay-line cal locks), not a strobe sweep.

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

// HEAP-PATTERN test: scattered small (4B) accesses over a large UNCACHED SBUS span -- the access mode
// a WAMR/Dart heap actually uses (NOT the sustained/consecutive bursts that fail). Each access is its
// own short CS#-low transaction with CS#-high gaps between (so the device self-refreshes). Returns the
// number of mismatches out of `n` scattered accesses. 0 => the PSRAM is usable as a heap (slow but
// correct), even though contiguous bursts cap at ~1KB. `interleave`=write+read each access in turn
// (true heap pattern), else write-all-then-read-all (also checks retention/self-refresh).
static uint32_t prv_psram_random_test(uint32_t span_bytes, uint32_t n, int interleave) {
  volatile uint32_t *base = (volatile uint32_t *)0x60000000u;
  const uint32_t words = span_bytes / 4u;
  uint32_t errs = 0u;
  if (interleave) {
    uint32_t lcg = 0x1234567u;
    for (uint32_t k = 0; k < n; k++) {
      lcg = lcg * 1103515245u + 12345u;
      uint32_t idx = (lcg >> 8) % words;
      base[idx] = 0xC0DE0000u ^ idx;
      __DSB();
      if (base[idx] != (0xC0DE0000u ^ idx)) errs++;
      if ((k & 0x3fu) == 0u) prompt_watchdog_feed();
    }
    return errs;
  }
  uint32_t lcg = 0x1234567u;
  for (uint32_t k = 0; k < n; k++) {
    lcg = lcg * 1103515245u + 12345u;
    uint32_t idx = (lcg >> 8) % words;
    base[idx] = 0xC0DE0000u ^ idx;  // value is a pure fn of idx, so write collisions are harmless
    if ((k & 0x3fu) == 0u) prompt_watchdog_feed();
  }
  __DSB();
  lcg = 0x1234567u;
  for (uint32_t k = 0; k < n; k++) {
    lcg = lcg * 1103515245u + 12345u;
    uint32_t idx = (lcg >> 8) % words;
    if (base[idx] != (0xC0DE0000u ^ idx)) errs++;
    if ((k & 0x3fu) == 0u) prompt_watchdog_feed();
  }
  return errs;
}

// CONTIGUOUS full-extent verify with ADDRESS-HASHED values. This is the readiness gate's heavy
// lifter: unlike the scattered random test (which only proves the access mode already known to pass),
// this writes then reads EVERY word over `span_bytes` in linear order -- the contiguous-burst pattern
// WAMR actually uses (memcpy of the module, GC mark/sweep). The value is a per-INDEX hash, so an
// addressing/aliasing fault (two indices mapping to one physical cell -- no size register on this
// part) makes the earlier index read back the later index's value => counted as a mismatch. `invert`
// flips polarity to catch coupling/ISI faults a single pattern misses. Returns the mismatch count
// over the WHOLE span. Run it over the full pool extent the consumer is handed, not a fraction.
static uint32_t prv_psram_contig_verify(uint32_t span_bytes, int invert) {
  volatile uint32_t *p = (volatile uint32_t *)PSRAM_TEST_BASE;
  const uint32_t words = span_bytes / 4u;
  const uint32_t flip = invert ? 0xFFFFFFFFu : 0u;
  for (uint32_t i = 0; i < words; i++) {
    p[i] = (i * 2654435761u) ^ flip;  // Knuth multiplicative hash of the index
    if ((i & 0xfffu) == 0u) prompt_watchdog_feed();
  }
  __DSB();
  uint32_t errs = 0u;
  for (uint32_t i = 0; i < words; i++) {
    if (p[i] != ((i * 2654435761u) ^ flip)) errs++;
    if ((i & 0xfffu) == 0u) prompt_watchdog_feed();
  }
  return errs;
}

// Multi-row SEPARATED survival: write `m` markers across m rows (16KB apart), then read them all
// back; return how many survived (0..m). This is the marginal access pattern (write-all-then-read).
// Used to score a DQS/SCK tap by REAL persistence, not the confounded contiguous/cached metric.
static uint32_t prv_psram_multirow_survive(uint32_t m) {
  volatile uint32_t *base = (volatile uint32_t *)0x60000000u;
  for (uint32_t s = 0; s < m; s++) {
    base[s * 4096u] = 0x6789ABCDu ^ (s * 0x01010101u);
    if ((s & 0x3fu) == 0u) prompt_watchdog_feed();
  }
  __DSB();
  uint32_t ok = 0u;
  for (uint32_t s = 0; s < m; s++) {
    if (base[s * 4096u] == (0x6789ABCDu ^ (s * 0x01010101u))) ok++;
    if ((s & 0x3fu) == 0u) prompt_watchdog_feed();
  }
  return ok;
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

  // REGISTER DUMP (research #1) -- FIRST emit so it always ships. The bulk-read cliff is likely a
  // CS#-timing / burst (tCSM) violation governed by DCR: a HyperRAM needs CS# to drop periodically
  // for self-refresh; CS# held too long (CSLMAX too high / RBSIZE too coarse) corrupts past a bound.
  {
    uint32_t dcr = s_psram_handle.Instance->DCR;
    uint32_t cr = s_psram_handle.Instance->CR;
    sniprintf(buf, sizeof(buf),
              "psram DCR=0x%08x RBSIZE=%u CSLMAX=%u CSLMIN=%u CSHMIN=%u TRCMIN=%u FIXLAT=%u PREFE=%u",
              (unsigned)dcr,
              (unsigned)((dcr & MPI_DCR_RBSIZE_Msk) >> MPI_DCR_RBSIZE_Pos),
              (unsigned)((dcr & MPI_DCR_CSLMAX_Msk) >> MPI_DCR_CSLMAX_Pos),
              (unsigned)((dcr & MPI_DCR_CSLMIN_Msk) >> MPI_DCR_CSLMIN_Pos),
              (unsigned)((dcr & MPI_DCR_CSHMIN_Msk) >> MPI_DCR_CSHMIN_Pos),
              (unsigned)((dcr & MPI_DCR_TRCMIN_Msk) >> MPI_DCR_TRCMIN_Pos),
              (unsigned)((dcr & MPI_DCR_FIXLAT_Msk) >> MPI_DCR_FIXLAT_Pos),
              (unsigned)((cr & MPI_CR_PREFE_Msk) >> MPI_CR_PREFE_Pos));
    emit(buf);
  }

  // === FAST DIAGNOSTIC (ships before the flaky no-bond console session drops) ===
  // Emit the decisive readiness signals over the short-lived session: a w0 write/read sanity check at
  // the cal's tap, a small FAST USABLE probe, then the HARDENED VERIFY gate below. Readiness is judged
  // by REAL array reads, NOT CALCR.DONE (which never asserts on this part even when reads are clean).
  {
    volatile uint32_t *sb = (volatile uint32_t *)PSRAM_TEST_BASE;
    sb[0] = 0xA5A50000u;
    sb[1] = 0x0000A5A5u;
    __DSB();
    sniprintf(buf, sizeof(buf), "psram FAST w0: wrote A5A50000,0000A5A5 read %08x,%08x",
              (unsigned)sb[0], (unsigned)sb[1]);
    emit(buf);
    // (REMOVED the HyperBus ReadID/ReadCR readback here -- it INTERMITTENTLY HANGS KernelBG, wedging
    // the diag right after w0 so the sweep never runs. We already have its values: CR0=0xe78f,
    // ID0=0x960c, CR1=0xc1ff. Judge by real array reads, not register reads.)
    uint32_t us = prv_psram_usable_at(PSRAM_TEST_BASE, 64u * 1024u);
    sniprintf(buf, sizeof(buf), "psram FAST USABLE (cal tap): SBUS=%uB", (unsigned)us);
    emit(buf);
    // Set the controller row boundary (RBSIZE=2) so CS# drops periodically during reads.
    HAL_FLASH_SET_ROW_BOUNDARY(&s_psram_handle, 2u);
    {
      // HARDENED VERIFY: gate on the access pattern AND extent the consumer actually uses -- WAMR runs
      // CONTIGUOUS bursts over the full 2MB pool, so a scattered/partial test would pass while bulk use
      // corrupts. Verify contiguous full-2MB + an inverse pass + scattered + multi-row, all clean.
      char r[180];
      const uint32_t SPAN = 2u * 1024u * 1024u;  // == the extent dart_runtime hands WAMR (0x60000000)
      // ISOLATION run (no margin sweep): gate at the cal's UNTOUCHED tap. The margin DQS sweep was
      // removed as the prime suspect -- perturbing DQS then restoring MISCR may not fully recover the
      // strobe, which would fail the gate spuriously. This run answers one question: does the
      // contiguous full-2MB test pass when the strobe is left exactly as the cal set it? The session
      // drops during the heavy passes, so the verdict is read indirectly via `dart test` (ready -> pool
      // armed -> sum=45 VERIFIED). The VERIFY emit below may not ship.
      uint32_t m = prv_psram_multirow_survive(16u);
      uint32_t c0 = prv_psram_contig_verify(SPAN, 0);
      uint32_t c1 = prv_psram_contig_verify(SPAN, 1);
      uint32_t rr = prv_psram_random_test(SPAN, 8000u, 0);
      uint32_t done = (s_psram_handle.Instance->CALCR & MPI_CALCR_DONE_Msk) ? 1u : 0u;
      uint32_t clk = HAL_QSPI_GET_CLK(&s_psram_handle);
      s_psram_ready = (m >= 16u && c0 == 0u && c1 == 0u && rr == 0u);
      sniprintf(r, sizeof(r),
                "psram VERIFY: clk=%uHz DONE=%u multirow=%u/16 contig=%u inv=%u rand=%u ready=%u",
                (unsigned)clk, (unsigned)done, (unsigned)m, (unsigned)c0, (unsigned)c1, (unsigned)rr,
                (unsigned)s_psram_ready);
      emit(r);
    }
  }
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
