"""Build a bootloader firmware image from a raw binary.

The output image is laid out as:

    offset 0                      40-byte image header
    offset 40                     padding (0xFF)
    offset BL_IMG_HDR_REGION      firmware payload

The payload begins with the application's Cortex-M vector table.
SCB->VTOR can only address a vector table aligned to the next power of
two at or above the table's size, so the payload must start on a
512-byte boundary rather than immediately after the 40-byte header.

These constants mirror core/include/bl_proto.h and must be kept in sync
with it; CI validates a generated image against the C definitions.
"""

import struct
import argparse
import zlib
from pathlib import Path

IMG_MAGIC = 0x4D494C42          # ASCII "BLIM", little-endian
HDR_VERSION = 1
HDR_STRUCT_SIZE = 40            # sizeof(bl_img_hdr_t)
HDR_REGION_SIZE = 512           # BL_IMG_HDR_REGION
WRITE_GRANULARITY = 8           # Target flash programming unit
PAD_BYTE = 0xFF                 # Matches erased flash

# struct.pack format for bl_img_hdr_t excluding the trailing hdr_crc32.
HDR_FORMAT_NO_CRC = "<IHHIIII12s"


def read_file(file_path):
    with open(file_path, "rb") as file:
        return file.read()


def pad_payload(data):
    """Pad the payload to a whole number of flash write granules.

    The target programs flash in fixed-size units, so a payload that is
    not a multiple of that unit cannot be written completely. Padding
    here keeps img_size equal to the number of bytes actually programmed,
    which in turn lets img_crc32 cover exactly the flash contents.
    """
    data = bytearray(data)
    remainder = len(data) % WRITE_GRANULARITY

    if remainder != 0:
        data.extend(bytes([PAD_BYTE]) * (WRITE_GRANULARITY - remainder))

    return bytes(data)


def parse_version(version):
    """Parse MAJOR.MINOR.PATCH into the packed fw_version encoding."""
    parts = version.split(".")

    if len(parts) != 3:
        raise ValueError("Version must be MAJOR.MINOR.PATCH, e.g. 1.2.3")

    try:
        major, minor, patch = map(int, parts)
    except ValueError:
        raise ValueError("Version must contain only numbers, e.g. 1.2.3")

    if not (0 <= major <= 255):
        raise ValueError("Major version must be 0-255")
    if not (0 <= minor <= 255):
        raise ValueError("Minor version must be 0-255")
    if not (0 <= patch <= 255):
        raise ValueError("Patch version must be 0-255")

    return (major << 16) | (minor << 8) | patch


def build_header(img_size, img_crc32, fw_version, entry_offset):
    """Build the 40-byte image header.

    hdr_crc32 covers the preceding 36 bytes, so the header is packed in
    two stages: the leading fields are serialised first, checksummed,
    and the checksum appended.
    """
    head = struct.pack(
        HDR_FORMAT_NO_CRC,
        IMG_MAGIC,
        HDR_VERSION,
        0,                  # flags
        img_size,
        img_crc32,
        fw_version,
        entry_offset,
        b"\x00" * 12,       # reserved
    )

    hdr_crc32 = zlib.crc32(head)
    header = head + struct.pack("<I", hdr_crc32)

    assert len(header) == HDR_STRUCT_SIZE

    return header, hdr_crc32


def build_header_region(header):
    """Pad the header out to the reserved region size.

    The padding is filled with the erased-flash value so that the region
    reads identically whether or not it has been programmed.
    """
    padding = bytes([PAD_BYTE]) * (HDR_REGION_SIZE - len(header))

    region = header + padding
    assert len(region) == HDR_REGION_SIZE

    return region


def write_file(file_path, header_region, payload):
    with open(file_path, "wb") as file:
        file.write(header_region)
        file.write(payload)


def main():
    parser = argparse.ArgumentParser(
        description="Create an STM32 bootloader image"
    )

    parser.add_argument("input", help="Input .bin file")
    parser.add_argument(
        "--version",
        required=True,
        help="Firmware version e.g. 1.2.3",
    )

    args = parser.parse_args()

    payload = read_file(args.input)

    if len(payload) == 0:
        parser.error("Input file is empty")

    payload = pad_payload(payload)
    img_crc32 = zlib.crc32(payload)

    try:
        fw_version = parse_version(args.version)
    except ValueError as error:
        parser.error(str(error))

    header, hdr_crc32 = build_header(
        len(payload),
        img_crc32,
        fw_version,
        HDR_REGION_SIZE,
    )

    header_region = build_header_region(header)

    output_path = Path(args.input).with_suffix(".blimg")
    write_file(output_path, header_region, payload)

    print("Input:", args.input)
    print("Output:", output_path)
    print("Version:", args.version, f"(0x{fw_version:08X})")
    print("Payload size:", len(payload))
    print("Header region:", HDR_REGION_SIZE)
    print("Total size:", HDR_REGION_SIZE + len(payload))
    print(f"Image CRC32: 0x{img_crc32:08X}")
    print(f"Header CRC32: 0x{hdr_crc32:08X}")


if __name__ == "__main__":
    main()
