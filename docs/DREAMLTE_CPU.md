# CPU on the Galaxy S8

This page holds the CPU facts of this phone, what the port does today, and what remains.

## Cores

The Exynos 8895 has two clusters:

| Cluster | Cores | MPIDR (Aff1) | Linux driver name | ACPM id |
| --- | --- | --- | --- | --- |
| Cortex-A53 | 4 | `0x100`-`0x103` | `dvfs_cpucl1` | `0x0B040003` |
| Mongoose M2 | 4 | `0x000`-`0x003` | `dvfs_cpucl0` | `0x0B040002` |

The A53 cluster boots first. S-Boot enters the loader on MPIDR `0x100`, at EL1, with the MMU off.

## What works

- Eight cores run XNU. Each Mongoose core starts with one PSCI `CPU_ON`. The run `K2c` logged `8/8 CPUs initialized, SGI1 verified`.
- All eight cores share one scheduler set, so the scheduler treats them as equal. This is a known weakness: the Mongoose cores are the fast ones.
- XNU uses the non-secure physical timer on PPI 14 (INTID 30). The EL1 virtual timer does not work on the secondary cores.
- The GIC is a GIC-400 at `0x10201000`, the timer runs at 26 MHz.

## Clocks

Samsung's Linux does not set CPU clocks itself. The APM firmware does, on request. The loader sends `FREQ_REQ id kHz` over the ACPM mailbox and the firmware sets the regulator from its own ASV table, then the PLL.

| Item | Address |
| --- | --- |
| ACPM mailbox | `0x16440000` |
| ACPM SRAM | `0x16500000`, 0xb400 |
| ACPM DVFS channel | channel 0 |
| PLL of cluster 0 | `0x16800000` + `0x120` |
| PLL of cluster 1 | `0x16900000` + `0x120` |

The rate is `26 MHz x M / (P x 2^S)`, with P = 3.

The loader asks for these rates and the kernel keeps them: A53 1690 MHz, Mongoose 1703 MHz. Run `c10` measured 1689-1690 MHz on all four A53 cores, run `c11` measured Mongoose at 1703 MHz.

`FREQ_GET` answers 0 on this firmware, so the kernel reads the PLLs to learn the current rate.

## Thermal

The early TMU is at `0x10080000`. Sensor code sits at `0x40` and the trim at `4 x sensor`. Sensors 0 and 2-5 are Mongoose, sensor 1 is A53. TWRP reads 37 C when idle.

The kernel guard steps both clusters down one level per second from 80 C, steps back up below 70 C, and returns to the S-Boot clocks at 95 C or on an implausible reading.

Run `c12` held 2002 MHz on the Mongoose cluster by stepping down at 80-82 C. That is too hot for a phone in the hand, so the daily setting stays at 1703 / 1690 MHz.

## What remains

1. A heterogeneous scheduler policy: UI work on the Mongoose cores, light work on the A53 cores.
2. Thermal policy above 80 C that keeps a usable frame rate.
3. Idle states and cluster power-down.
4. ASV-specific voltage limits read from the ECT at boot, instead of fixed tables.
