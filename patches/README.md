# Local patches to vendored submodules

These are local patches we carry against vendored upstream submodules we do **not**
own (so they are version-controlled here, in our repo, but never pushed upstream — per
the project's hard rule: no PR/push to upstreams we don't own without explicit approval).

The build expects them applied to the working tree. They are kept as `.patch` files so a
fresh `git submodule update` can re-apply them and so the change is reviewable.

## sifli-hal-bound-mpi-spins.patch

Target: `third_party/hal_sifli/SiFli-SDK` (`coredevices/SiFli-SDK`), files
`drivers/hal/bf0_hal_mpi_psram.c` and `drivers/hal/bf0_hal_mpi.c`.

Bounds the **two unbounded hardware-status spins** reachable from `HAL_MPI_PSRAM_Init` on
the HBPSRAM (Winbond HYPERBUS, obelix) path, both of which can wedge KernelBG forever (and
trigger the watchdog reboot loop) when the part doesn't respond after a clock/DQS reconfigure:

1. **`HAL_MPI_OPSRAM_CAL_DELAY`** (`bf0_hal_mpi_psram.c`): the read-strobe auto-calibration
   `while(!(CALCR & DONE));`.
2. **`HAL_FLASH_SET_CMD`** (`bf0_hal_mpi.c`): the manual-command `while(!(SR & TCF));` — hit
   during init via `HAL_HYPER_PSRAM_WriteCR` (the CR0 write). This was the observed init hang.

Both are capped with a generous counter; on timeout init falls through (callers ignore the
return / the off-center tap is overridden by our explicit tap sweep in
`src/fw/soc/sf32lb/sf32lb52x/psram.c`). A truly dead part is then caught by the WDTR-bounded
read-back test instead of hanging. Confirmed effective (not ROM-shadowed): this tree builds
with waf, so `ROM_ENABLED` is undefined and `__HAL_ROM_USED` expands empty -> these are plain
strong definitions compiled from source.

Apply / re-apply / remove:
```sh
cd third_party/hal_sifli/SiFli-SDK
git apply ../../../patches/sifli-hal-bound-mpi-spins.patch        # apply
git apply -R ../../../patches/sifli-hal-bound-mpi-spins.patch     # revert
git apply --check ../../../patches/sifli-hal-bound-mpi-spins.patch # dry-run / verify applies
```
(The submodule is at a detached HEAD; `git -C third_party/hal_sifli/SiFli-SDK status`
will show the files as modified once applied — that's expected and intentional.)
