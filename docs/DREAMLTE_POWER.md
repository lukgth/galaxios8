# Power and battery on the Galaxy S8

The battery path uses the Maxim MAX77865 companion chip. This page holds what the port reads today and what remains.

## Hardware facts

| Item | Value |
| --- | --- |
| Fuel gauge | MAX77865, `hsi2c_32`, node `max77865-fuelgauge` |
| Charger control | the same MAX77865 through the SPEEDY bus of the S2MPS17 |
| RTC | S2MPS17, `rtc@16490000`, read by the loader |
| Charger detection node | `cable-info` in the device tree |
| Battery node | `battery` in the device tree |

## What works today

- The kernel reads the fuel gauge in `bsd/dev/a5xelte_touch.c` (`dreamlte_battery_read`) and publishes level, voltage, current, external power and charging state through `SweetBattery`.
- Boot 29 read 99 percent and 4149 mV from the real gauge.
- The loader reads the RTC and passes the time to XNU, so the clock is correct at boot.

## What remains

1. Charging must stay enabled while iOS runs. The charger is controlled by the MAX77865 through the PMIC, and no code sets a charge current today.
2. Verify that "charger detected" and "current flows into the battery" are two different readings. Report them separately.
3. Battery temperature and the full state from the gauge.
4. Remove the constant fallback values. A transient retry at boot is acceptable, a fixed percentage is not.
5. Measure a 30 minute charge with iOS running: level must rise, and the charger must not be limited to a trickle.

## How to read the state

Read the gauge from TWRP with the same registers that the kernel uses, then compare with what iOS shows. If they differ, the fault is in the presentation and not in the gauge.
