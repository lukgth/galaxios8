# Metal on the Galaxy S8

Status: no Metal work has started. This page records the plan and its limits.

## Why Apple's Metal cannot run

The iPhone X has an Apple GPU with the AGX driver stack. The Galaxy S8 has a Mali-G71. Apple's kernel driver and user space driver talk to AGX hardware and to no other GPU. Do not emulate AGX.

## The path that can work

Translate the API below Metal, and translate the shaders to a form that Mesa can compile for Bifrost:

1. A compatibility layer answers the Metal API calls that the system components use.
2. An AIR front end reads Apple's shader intermediate code from the application.
3. The front end lowers AIR to Mesa NIR.
4. Mesa Panfrost compiles the NIR for Bifrost.
5. Panfrost submits work to the S8Mali driver.

The prior art is HackiOS (`oosexgang/hackios-xnu-userland`, `tools/air/`, `tools/metal/`, `docs/air.md`). Their Metal plugin loads on iOS 16 with a different GPU, so the API side is plausible. Their shader front end covers part of the system shaders, and their backend glue is tied to Adreno.

## Order of work

Metal comes after accelerated OpenGL ES. CoreAnimation reaches the GPU through its OpenGL ES backend, which needs no Metal.

| Step | Exit test |
| --- | --- |
| M0 | no Metal (today) |
| M1 | basic Metal objects answer, with no rendering |
| M2 | AIR is parsed |
| M3 | AIR becomes valid NIR |
| M4 | Panfrost compiles AIR-derived NIR |
| M5 | the first Metal triangle |
| M6 | textures and simple pipelines |
| M7 | stock components that need Metal run |
| M8 | broad compatibility |

## Rules

- Report which renderer drew a frame. A frame from llvmpipe is not a Metal success.
- Keep the AIR work in user space. The kernel driver serves Mesa and knows nothing about AIR.
