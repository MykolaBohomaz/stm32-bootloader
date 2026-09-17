#!/usr/bin/env python3
"""Run a full update and a rollback against a simulated device.

Everything below happens through the real bootloader core, loaded as a
shared library, and the real host tool. No hardware is involved and
nothing is mocked: the same code paths run here as on a device.

    python3 tools/demo.py
"""

import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import blflash
import blimage
import blproto
import blsim

PAYLOAD_SIZE = 8192
INITIAL_MSP = 0x20010000


def build_image(app_base, fw_version):
    """Build an image whose payload opens with a usable vector table."""
    payload = bytearray((i * 7 + 3) & 0xFF for i in range(PAYLOAD_SIZE))

    payload[0:4] = INITIAL_MSP.to_bytes(4, "little")
    payload[4:8] = ((app_base + 0x100) | 1).to_bytes(4, "little")

    payload = bytes(payload)

    header, _ = blimage.build_header(
        len(payload),
        zlib.crc32(payload) & 0xFFFFFFFF,
        fw_version,
        blimage.HDR_REGION_SIZE,
    )

    padding = bytes([blimage.PAD_BYTE]) * (
        blimage.HDR_REGION_SIZE - len(header)
    )

    return header + padding + payload


def heading(text):
    print()
    print(text)
    print("-" * len(text))


def show_state(device):
    meta = device.metadata()

    if meta is None:
        print("  boot metadata: none — the bootloader selects by version")
        return

    active, state, attempts = meta

    print(
        f"  boot metadata: slot {blproto.slot_name(active)}, "
        f"{blproto.BOOT_STATE_NAMES.get(state, state)}, "
        f"attempts {attempts}"
    )


def boot(device, label):
    target = device.boot()

    if target == 0:
        print(f"  {label}: nothing bootable")
        return None

    for slot in (blproto.SLOT_A, blproto.SLOT_B):
        if target == device.slot_base(slot) + blimage.HDR_REGION_SIZE:
            print(
                f"  {label}: booted slot {blproto.slot_name(slot)} "
                f"at 0x{target:08X}"
            )
            return slot

    print(f"  {label}: booted 0x{target:08X}")

    return None


def main():
    try:
        device = blsim.SimulatedDevice()
    except (blsim.SimUnavailable, OSError) as error:
        print(error, file=sys.stderr)
        print(
            "\nBuild the host configuration first:\n"
            "  cmake -B build && cmake --build build",
            file=sys.stderr,
        )
        return 1

    device.factory_reset()
    bootloader = blflash.Bootloader(device, log=lambda message: None)

    heading("A pristine device")
    info = bootloader.hello()
    print(f"  protocol v{info.proto_version}, image header "
          f"v{info.hdr_version}")
    print(f"  {info.slot_count} slots of {info.slot_size} bytes")
    print(f"  write granularity {info.write_granularity}, "
          f"erase granularity {info.erase_granularity}")
    show_state(device)
    boot(device, "boot")

    heading("Installing v1.0.0")
    image_v1 = build_image(
        device.slot_base(blproto.SLOT_A) + blimage.HDR_REGION_SIZE,
        0x00010000,
    )
    slot_v1 = bootloader.update(image_v1)
    print(f"  installed into slot {blproto.slot_name(slot_v1)}")
    show_state(device)

    device.reboot()
    boot(device, "boot 1")
    device.confirm()
    print("  the image confirmed itself healthy")
    show_state(device)

    heading("Installing v2.0.0, which will fail to confirm")
    image_v2 = build_image(
        device.slot_base(blproto.SLOT_B) + blimage.HDR_REGION_SIZE,
        0x00020000,
    )
    slot_v2 = bootloader.update(image_v2)
    print(f"  installed into slot {blproto.slot_name(slot_v2)} — "
          f"the active slot was preserved")
    show_state(device)

    for attempt in range(1, 4):
        device.reboot()
        boot(device, f"boot {attempt}")
        show_state(device)

    heading("Attempts exhausted")
    device.reboot()
    booted = boot(device, "boot 4")
    show_state(device)

    if booted == slot_v1:
        print("  the bootloader reverted to the previously confirmed image")
    else:
        print("  unexpected: no rollback occurred")
        return 1

    heading("An interrupted update")
    header = blimage.decode_header(image_v2)
    payload = image_v2[header.entry_offset:]

    bootloader.erase_slot(blproto.SLOT_B)

    sent = 0
    while sent < len(payload) // 2:
        chunk = payload[sent:sent + blproto.MAX_WRITE_DATA]
        bootloader.write(header.entry_offset + sent, chunk)
        sent += len(chunk)

    print(f"  wrote {sent} of {len(payload)} bytes, then stopped before "
          f"committing the header")

    status = bootloader.slot_status(blproto.SLOT_B)
    print(f"  slot B now reports {blproto.status_name(status)}")

    device.reboot()
    booted = boot(device, "boot")

    if booted == slot_v1:
        print("  the previously confirmed image still runs")
    else:
        print("  unexpected: the device did not fall back")
        return 1

    heading("A corrupted image")
    device.corrupt(
        device.slot_base(slot_v1) + blimage.HDR_REGION_SIZE + 64, 8
    )
    status = bootloader.slot_status(slot_v1)
    print(f"  slot {blproto.slot_name(slot_v1)} now reports "
          f"{blproto.status_name(status)}")

    device.reboot()
    boot(device, "boot")

    print()
    print("Done. Every step above ran through the bootloader core and the")
    print("host tool, with a simulated flash device standing in for the")
    print("target's memory.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
