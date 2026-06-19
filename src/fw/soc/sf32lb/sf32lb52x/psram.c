/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! PSRAM bring-up for obelix (SF32LB52J: 16 MB OPI-PSRAM on MPI1 @ 0x60000000).
//! init.c parks PSRAM at boot (disables the 1.8 V LDO, sets PAD_SA00-SA12
//! analog). This un-parks it and initializes the MPI1 PSRAM controller so it can
//! back the Dart/WAMR pool. Driven by the `psram` console command so it can be
//! tuned headlessly (clock divider) without auto-running at boot.
//!
//! Sequence templated from SiFli's sf32lb52-lcd_base board_init_psram() +
//! common/flash.c bsp_psramc_init() + sf32lb52-lcd_base bsp_pinmux.c.

#include <bf0_hal.h>

#include "console/prompt.h"
#include "system/logging.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef PSRAM_TEST_BASE
#define PSRAM_TEST_BASE 0x60000000u  // MPI1 SBUS alias (CBUS 0x10000000 + 0x50000000)
#endif
#define PSRAM_MSIZE_MB 16            // SF32LB52J embedded PSRAM

static FLASH_HandleTypeDef s_psram_handle;
static bool s_psram_ready;

//! Restore the octal PSRAM pin mux (undo init.c's HAL_PIN_Set_Analog parking).
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
  HAL_PIN_Set(PAD_SA00, MPI1_DM, PIN_PULLDOWN, 1);
  HAL_PIN_Set(PAD_SA06, MPI1_CLKB, PIN_NOPULL, 1);
  HAL_PIN_Set(PAD_SA12, MPI1_DQSDM, PIN_PULLDOWN, 1);
}

//! Bring up the MPI1 PSRAM controller. div = MPI1 clock divider (sweepable).
//! Returns HAL_OK on success; fills *pid with the detected PSRAM type id.
static HAL_StatusTypeDef prv_psram_init(uint16_t div, uint32_t *pid_out) {
  // 1. Re-enable the 1.8 V LDO that feeds PSRAM (undo init.c's power-down).
  hwp_pmuc->PERI_LDO &= ~PMUC_PERI_LDO_LDO18_PD_Msk;
  hwp_pmuc->PERI_LDO |= PMUC_PERI_LDO_EN_LDO18_Msk;
  HAL_Delay_us(150);

  // 2. Restore the PSRAM pin mux.
  prv_restore_pinmux();

  // 3. Enable DLL2 (288 MHz) and run MPI1 (FLASH1) off it, exactly like the SiFli
  //    boot path for PSRAM (bsp_init.c: EnableDLL2(288M) -> ClockSelect FLASH1 DLL2,
  //    mpi1_div=2 -> 144 MHz). SYSCLK lets the controller init (HAL_OK) but HYPERBUS
  //    reads come back wrong; the PSRAM calibrates against the DLL2 clock. DLL2 must
  //    be ENABLED before selecting it (else MPI1 has no clock and the init hangs).
  HAL_RCC_HCPU_EnableDLL2(288000000);
  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_DLL2);

  // Feed the KernelBG watchdog: the controller init/calibration can be slow.
  prompt_watchdog_feed();

  // 4. Detect PSRAM type from the chip ID register + init the controller.
  uint32_t pid = (hwp_hpsys_cfg->IDR & HPSYS_CFG_IDR_PID_Msk) >> HPSYS_CFG_IDR_PID_Pos;
  pid &= 7;
  if (pid_out) {
    *pid_out = pid;
  }

  qspi_configure_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.Instance = hwp_qspi1;  // MPI1
  cfg.msize = PSRAM_MSIZE_MB;
  cfg.base = QSPI1_MEM_BASE;  // 0x10000000 (CBUS)
  switch (pid) {
    case 5: cfg.SpiMode = SPI_MODE_PSRAM; break;     // 16Mb APM QSPI
    case 4: cfg.SpiMode = SPI_MODE_LEGPSRAM; break;  // 32Mb LEGACY
    case 6: cfg.SpiMode = SPI_MODE_HBPSRAM; break;   // Winbond HYPERBUS
    case 2:                                          // 128Mb (=16MB) XCELLA OPI
    case 3:                                          // 64Mb XCELLA OPI
    default: cfg.SpiMode = SPI_MODE_OPSRAM; break;   // SF32LB52J is OPI
  }

  memset(&s_psram_handle, 0, sizeof(s_psram_handle));
  return HAL_MPI_PSRAM_Init(&s_psram_handle, &cfg, div);
}

//! Write/read sanity test over the PSRAM window. Returns -1 + the failing index,
//! else the number of words verified.
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

//! Console command: `psram <div> [dqs]`. div = MPI1 clock divider (default 2).
//! HYPERBUS (HBPSRAM) is NOT DQS-calibrated by HAL_MPI_PSRAM_Init (calibration is
//! OPSRAM-only), so HYPERBUS reads come back wrong with the default DQS delay. Pass
//! [dqs] (0..31) to set the DQS delay manually and sweep it headlessly to find the
//! value that makes the write/read test pass; >31 or omitted = HAL default (no set).
void command_psram(const char *div_str, const char *dqs_str) {
  char buf[128];
  uint16_t div = 2;
  if (div_str && div_str[0]) {
    int v = atoi(div_str);
    if (v >= 1 && v <= 16) {
      div = (uint16_t)v;
    }
  }
  int dqs = -1;
  if (dqs_str && dqs_str[0]) {
    dqs = atoi(dqs_str);
  }

  uint32_t pid = 0xff;
  HAL_StatusTypeDef res = prv_psram_init(div, &pid);
  if (res == HAL_OK && dqs >= 0 && dqs <= 31) {
    HAL_MPI_ENABLE_DQS(&s_psram_handle, 1);
    HAL_MPI_SET_DQS_DELAY(&s_psram_handle, (uint8_t)dqs);
  }
  prompt_send_response_fmt(buf, sizeof(buf), "psram init div=%u pid=%u dqs=%d -> %s",
                           (unsigned)div, (unsigned)pid, dqs,
                           res == HAL_OK ? "HAL_OK" : "HAL_ERR");
  if (res != HAL_OK) {
    return;
  }

  uint32_t fail = 0;
  int n = prv_psram_test(256, &fail);
  if (n < 0) {
    prompt_send_response_fmt(buf, sizeof(buf), "psram test FAIL at word %u", (unsigned)fail);
  } else {
    s_psram_ready = true;
    prompt_send_response_fmt(buf, sizeof(buf),
                             "psram test OK (%d words @0x%08x) - %u MB ready",
                             n, (unsigned)PSRAM_TEST_BASE, PSRAM_MSIZE_MB);
  }
}
