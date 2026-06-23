/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! Pure (no firmware deps) read-strobe tap-window selection for the PSRAM DQS/SCK
//! sweep. Given a pass/fail map across candidate taps, pick the CENTER of the WIDEST
//! contiguous passing window -- the canonical HyperRAM/OctoSPI calibration result
//! (the center of a passing run is the most timing-robust tap). Kept dependency-free
//! so it is unit-testable on the host (see tests/test_psram_tapwindow.c).

#ifndef PSRAM_TAPWINDOW_H
#define PSRAM_TAPWINDOW_H

//! pass[i] != 0 means tap i read back correctly. Returns the center index of the
//! widest contiguous run of passing taps, or -1 if none pass. On success, *out_lo /
//! *out_hi (when non-NULL) get the inclusive bounds of that window. On a tie in
//! width, the FIRST (lowest-tap) widest window wins.
static inline int psram_pick_tap_center(const unsigned char *pass, int n,
                                        int *out_lo, int *out_hi) {
  int best_lo = -1, best_hi = -1, best_w = -1;
  int run_lo = -1;
  for (int i = 0; i < n; i++) {
    if (pass[i]) {
      if (run_lo < 0) {
        run_lo = i;
      }
    } else if (run_lo >= 0) {
      int w = (i - 1) - run_lo;  // width-1 (0 for a single-tap run); fine for compare
      if (w > best_w) {
        best_w = w;
        best_lo = run_lo;
        best_hi = i - 1;
      }
      run_lo = -1;
    }
  }
  if (run_lo >= 0) {  // run reaches the end
    int w = (n - 1) - run_lo;
    if (w > best_w) {
      best_w = w;
      best_lo = run_lo;
      best_hi = n - 1;
    }
  }
  if (best_lo < 0) {
    if (out_lo) {
      *out_lo = -1;
    }
    if (out_hi) {
      *out_hi = -1;
    }
    return -1;
  }
  if (out_lo) {
    *out_lo = best_lo;
  }
  if (out_hi) {
    *out_hi = best_hi;
  }
  return (best_lo + best_hi) / 2;
}

#endif  // PSRAM_TAPWINDOW_H
