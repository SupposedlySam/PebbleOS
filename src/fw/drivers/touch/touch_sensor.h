/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

void touch_sensor_init(void);

//! Diagnostic: number of touch-controller interrupts since boot (0 on stubs).
uint32_t touch_sensor_diag_isr_count(void);

//! Diagnostic: I2C read failures in the interrupt-drain path (0 on stubs).
uint32_t touch_sensor_diag_read_fail_count(void);

//! Enable or disable touch sensor interrupts.
//! When disabled, no touch events will be processed.
//! @param enabled true to enable, false to disable
void touch_sensor_set_enabled(bool enabled);
