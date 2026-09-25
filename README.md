# STM32 Bootloader

[![CI](https://github.com/MykolaBohomaz/stm32-bootloader/actions/workflows/ci.yml/badge.svg)](https://github.com/MykolaBohomaz/stm32-bootloader/actions/workflows/ci.yml)

A dual-slot firmware updater for STM32 that validates every image before
booting it and automatically reverts to the previous version when new
firmware fails to start.

**Status:** host-tested; on-target port in progress.

## Flash layout — STM32L432KC, 256 KiB

```
0x08000000  bootloader        32 KiB
0x08008000  metadata A         2 KiB   ┐ ping-pong: one record always
0x08008800  metadata B         2 KiB   ┘ survives an interrupted write
0x08009000  slot A           108 KiB   application at +0x200
0x08024000  slot B           108 KiB   application at +0x200
0x0803F000  reserved           4 KiB
```

Each slot holds a 40-byte header — magic, sizes, two CRC-32s, version —
in a reserved 512-byte region. The header is padded to 512 rather than
packed tight because `SCB->VTOR` can only address a vector table aligned
to a power of two at or above its size.

The header is written **last**, making it the commit point: an update
interrupted before that leaves a slot with no valid header, which is
cleanly rejected instead of looking valid over a half-written payload.

## Trial boot and rollback

```
          SET_ACTIVE                 app calls confirm()
   ──────────────────────▶ TRIAL ──────────────────────▶ CONFIRMED
                            │  ▲                             │
            each boot: n+1  │  │ n < 3                       │ boots freely,
                            ▼  │                             ▼ no counting
                        ┌───────────┐                   ┌──────────┐
                        │ attempts  │                   │  runs    │
                        └───────────┘                   └──────────┘
                            │ n = 3
                            ▼
                    revert to the other slot, mark CONFIRMED
```

The counter is incremented **before** control is transferred, so an image
that hangs, faults or resets without confirming still consumes an
attempt. Rollback is skipped when the other slot holds no valid image —
booting a failing image beats booting nothing.

## Wire protocol

```
SOF 0x7E │ CMD │ LEN (2, LE) │ PAYLOAD ≤512 │ CRC-32 (4, LE)
```

The CRC covers `CMD`, `LEN` and `PAYLOAD`. The length field arrives from
outside and is bounds-checked before a single payload byte is stored.
`0x7E` is not escaped: a desynchronised receiver loses at most one frame
before the CRC rejects it and it resynchronises.

Every request is answered, including failures — silence is reserved for
a device that is absent. Responses echo the request's command byte and
lead with a status code.

`HELLO` · `ERASE_SLOT` · `WRITE` · `VERIFY` · `SET_ACTIVE` · `RESET`

Writes must ascend, with one exception at offset 0 for the header
commit. The target cannot program a location twice between erases, and a
retransmission after a lost acknowledgement is ordinary behaviour, so
the device enforces the ordering rather than trusting the host.

## Build and test

```bash
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure    # 186 tests
```

C tests build under AddressSanitizer and UndefinedBehaviorSanitizer with
`-Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion`.

```bash
python3 tools/demo.py    # install, confirm, fail a trial, watch it revert
```

The demo runs the real bootloader core, compiled as a shared library
behind a simulated flash device, driven by the real host tool.

## Design

`core/` is hardware-independent and compiles unchanged for both host and
target; CI rejects any hardware dependency that appears in it. Below it,
`bl_port.h` is a 13-function seam with two implementations — a simulated
device used by the tests, and the STM32 port.

The simulator is deliberately **stricter** than real flash: it rejects
misaligned writes, programming a location twice between erases, and
implausible vector tables, and it can fail flash from the *n*th write
onward so power loss during an update is reproducible.

```
core/       bl_core  bl_meta  bl_frame  bl_crc32  bl_api
port/host/  simulated flash, fault injection
tools/      mkimage.py  blflash.py  demo.py
```

## Roadmap

- [x] Image format, framing, boot selection, trial boot and rollback
- [x] Host tool and end-to-end tests against the real core
- [ ] STM32L4 port, linker scripts, jump to application
- [ ] USB CDC transport, STM32F4 port
