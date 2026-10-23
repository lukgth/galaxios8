# Wi-Fi on the Galaxy S8

Wi-Fi is a Broadcom combo chip behind PCIe. No driver exists in this port yet. This page records what is known from the phone and what the work needs.

## Hardware facts measured on the phone

Read from the TWRP device tree on 2026-10-23.

| Item | Value |
| --- | --- |
| WLAN node | `wlan`, `compatible = "samsung,brcm-wlan"` |
| WLAN control GPIOs | two, one with pin 3 and one with pin 7, both active high |
| PCIe controller 1 | `pcie1@116B0000`, `compatible = "samsung,exynos8895-pcie"`, one lane |
| PCIe controller 0 | `pcie0@116A0000`, `compatible = "samsung,exynos-pcie"` |
| Bluetooth node | `bluetooth`, `compatible = "samsung,bcm43xx"` |

The chip family is Broadcom BCM43xx, on the same combo die as Bluetooth. The exact chip id and the firmware come from the vendor files on the phone's Android system partition, which this project can read in TWRP.

## What a driver needs

1. Power: the regulators and the control GPIOs of the WLAN node. Read the values from Samsung's kernel source, do not guess.
2. PCIe: enumeration of the Exynos8895 PCIe controller 1, one lane, with the PHY and the clock.
3. Firmware: the chip firmware, the NVRAM file and the calibration data. They live in the vendor partition.
4. Transport: the Broadcom protocol over PCIe, as Linux `brcmfmac` implements it.
5. Network: a BSD network interface in XNU. DHCP and DNS then work without the iOS UI.

## Milestones

| Step | Exit test |
| --- | --- |
| W0 | hardware identified (done: this page) |
| W1 | power and PCIe enumeration, the chip answers its config space |
| W2 | firmware loaded into the chip |
| W3 | firmware answers over the transport |
| W4 | scan returns access points |
| W5 | association with WPA2 |
| W6 | DHCP lease |
| W7 | IPv4 traffic, DNS answers |
| W8 | Safari loads a page |
| W9 | the iOS Wi-Fi settings show the network |

USB networking does not count as Wi-Fi.
