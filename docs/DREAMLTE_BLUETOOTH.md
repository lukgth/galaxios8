# Bluetooth on the Galaxy S8

Bluetooth is a Broadcom BCM43xx combo chip. No driver exists in this port yet.

## Hardware facts measured on the phone

Read from the TWRP device tree on 2026-10-23.

| Item | Value |
| --- | --- |
| Node | `bluetooth`, `compatible = "samsung,bcm43xx"`, status `okay` |
| GPIOs | three control lines, pins 7, 4 and 3, plus a pin control group |
| Shared die | the same combo chip as Wi-Fi (`samsung,brcm-wlan`) |

The transport is a UART HCI link on this family. Confirm the exact UART node and the baud rate in Samsung's kernel source before you write code.

## What a driver needs

1. Power: the enable and wake lines, and the regulator of the combo chip.
2. UART: the right controller, the pin mux and the baud rate. Broadcom parts start at a low rate and switch after a command.
3. Firmware: the BCM43xx HCD file and the NVRAM file from the vendor files on the phone.
4. HCI: a small transport in XNU with the command, event and ACL queues.
5. Coexistence: the combo chip arbitrates between Wi-Fi and Bluetooth. Read the vendor settings before you enable both.

## Milestones

| Step | Exit test |
| --- | --- |
| B0 | chip powered |
| B1 | HCI `Reset` command answers with a command complete event |
| B2 | firmware loaded if the part needs it |
| B3 | discovery returns a nearby device |
| B4 | pairing completes |
| B5 | a HID device works |
| B6 | Bluetooth audio works |

The iOS settings UI is not a precondition for B1 to B5.
