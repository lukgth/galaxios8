# Audio on the Galaxy S8

The audio path is Samsung ABOX with a Cirrus Logic Madera codec and a smart amplifier. No audio driver exists in this port yet.

## Hardware facts measured on the phone

Read from the TWRP device tree on 2026-10-23.

| Item | Value |
| --- | --- |
| Sound card node | `sound`, `compatible = "samsung,exynos8895-madera"` |
| ABOX | `abox@0x13E50000`, `compatible = "samsung,abox"` |
| ABOX GIC | `abox_gic@0x13EF0000` |
| ABOX read channels | eight `abox_rdma` at `0x13E51000` and up, one every 0x100 |
| ABOX write channels | eight `abox_wdma` at `0x13E52000` and up, one every 0x100 |
| ABOX effect block | `abox_effect@0x13F2E000` |
| Dummy codec | `audio_codec_dummy`, `compatible = "snd-soc-dummy"` |

The ABOX is an audio DSP with its own interrupt controller and its own SRAM. The codec and the amplifier attach to it over I2S and I2C, so the DSP and its firmware come first.

## Milestones

| Step | Exit test |
| --- | --- |
| A0 | topology documented (this page) |
| A1 | ABOX power domain and clocks on |
| A2 | the codec answers on its control bus |
| A3 | the amplifier is set with the values of Samsung's driver |
| A4 | one PCM test tone through a write channel |
| A5 | speaker playback from XNU |
| A6 | a CoreAudio bridge in the kernel |
| A7 | system sounds |
| A8 | media playback |
| A9 | earpiece |
| A10 | headphones |
| A11 | microphone capture |

## Rules

- Do not raise the amplifier gain without the values from Samsung's working driver.
- Read the ABOX firmware layout from Samsung's kernel source before writing to the DSP.
- The DBGDEV power domains of the DSP and the ABOX are listed in the device tree of TWRP.
