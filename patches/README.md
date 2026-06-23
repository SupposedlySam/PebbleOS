# Local patches to vendored submodules

These are local patches we carry against vendored upstream submodules we do **not**
own (so they are version-controlled here, in our repo, but never pushed upstream — per
the project's hard rule: no PR/push to upstreams we don't own without explicit approval).

The build expects them applied to the working tree. They are kept as `.patch` files so a
fresh `git submodule update` can re-apply them and so the change is reviewable.

## sifli-hal-bound-cal-spin.patch

Target: `third_party/hal_sifli/SiFli-SDK` (`coredevices/SiFli-SDK`),
file `drivers/hal/bf0_hal_mpi_psram.c`.

Bounds the PSRAM read-strobe auto-calibration spin in `HAL_MPI_OPSRAM_CAL_DELAY`. The
upstream code is `while(!(CALCR & DONE));` with **no timeout** — if the calibration DONE
bit never asserts (wrong strobe / non-responding part) it wedges KernelBG forever and the
watchdog reboot-loops. The patch caps the spin; on timeout init falls through (the
off-center tap it leaves is overridden by our explicit tap sweep in
`src/fw/soc/sf32lb/sf32lb52x/psram.c` anyway). This keeps PSRAM bring-up from ever
wedging the watch.

Apply / re-apply / remove:
```sh
cd third_party/hal_sifli/SiFli-SDK
git apply ../../../patches/sifli-hal-bound-cal-spin.patch        # apply
git apply -R ../../../patches/sifli-hal-bound-cal-spin.patch     # revert
git apply --check ../../../patches/sifli-hal-bound-cal-spin.patch # dry-run / verify applies
```
(The submodule is at a detached HEAD; `git -C third_party/hal_sifli/SiFli-SDK status`
will show the file as modified once applied — that's expected and intentional.)
