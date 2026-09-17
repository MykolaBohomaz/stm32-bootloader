"""Bootloader firmware image format.

Mirrors the image header definitions in core/include/bl_proto.h. The two
definitions are independent implementations of one format, so
tools/check_sync.py compares them in CI and fails if they drift apart.

An image occupies a slot as:

    offset 0            40-byte image header
    offset 40           padding (0xFF)
    HDR_REGION_SIZE     firmware payload

The payload begins with the application's Cortex-M vector table.
SCB->VTOR can only address a vector table aligned to the next power of
two at or above the table's size, so the payload starts on a 512-byte
boundary rather than immediately after the 40-byte header.
"""

import struct
import zlib
from collections import namedtuple

IMG_MAGIC = 0x4D494C42          # ASCII "BLIM", little-endian
HDR_VERSION = 1
HDR_STRUCT_SIZE = 40            # sizeof(bl_img_hdr_t)
HDR_REGION_SIZE = 512           # BL_IMG_HDR_REGION
VTOR_MIN_ALIGNMENT = 128        # BL_VTOR_MIN_ALIGNMENT
WRITE_GRANULARITY = 8           # Target flash programming unit
PAD_BYTE = 0xFF                 # Matches erased flash

# struct.pack format for bl_img_hdr_t, with and without the trailing
# hdr_crc32. The leading '<' sets little-endian and disables padding;
# without it Python would insert alignment bytes and the header would no
# longer be 40 bytes.
HDR_FORMAT_NO_CRC = "<IHHIIII12s"
HDR_FORMAT_WITH_CRC = "<IHHIIII12sI"

ImageHeader = namedtuple(
    "ImageHeader",
    "magic hdr_version flags img_size img_crc32 fw_version "
    "entry_offset reserved hdr_crc32",
)


def format_version(fw_version):
    """Render a packed fw_version as MAJOR.MINOR.PATCH."""
    return (
        f"{(fw_version >> 16) & 0xFF}."
        f"{(fw_version >> 8) & 0xFF}."
        f"{fw_version & 0xFF}"
    )


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

    hdr_crc32 = zlib.crc32(head) & 0xFFFFFFFF
    header = head + struct.pack("<I", hdr_crc32)

    assert len(header) == HDR_STRUCT_SIZE

    return header, hdr_crc32


def decode_header(header):
    """Decode the 40-byte header without validating it."""
    if len(header) < HDR_STRUCT_SIZE:
        raise ValueError("buffer shorter than the image header")

    return ImageHeader(
        *struct.unpack(HDR_FORMAT_WITH_CRC, header[:HDR_STRUCT_SIZE])
    )


def validate(image, slot_size=None):
    """Validate an image the way the bootloader will, and return its header.

    Checking locally matters because the device must erase a slot before
    it can be written: an image rejected after the erase has cost the
    user a slot, while one rejected here costs nothing.

    Raises ValueError with the reason on failure.
    """
    if len(image) < HDR_REGION_SIZE:
        raise ValueError("image shorter than the reserved header region")

    header = decode_header(image)

    if header.magic != IMG_MAGIC:
        raise ValueError(f"bad magic 0x{header.magic:08X}")

    if header.hdr_version != HDR_VERSION:
        raise ValueError(
            f"unsupported header version {header.hdr_version}, "
            f"expected {HDR_VERSION}"
        )

    expected_hdr_crc = zlib.crc32(image[:HDR_STRUCT_SIZE - 4]) & 0xFFFFFFFF

    if header.hdr_crc32 != expected_hdr_crc:
        raise ValueError("header CRC mismatch")

    # Only now may the header's own fields be trusted.
    if header.entry_offset < HDR_STRUCT_SIZE:
        raise ValueError(
            f"entry offset {header.entry_offset} overlaps the header"
        )

    if header.entry_offset % HDR_REGION_SIZE != 0:
        raise ValueError(
            f"entry offset {header.entry_offset} would misalign the "
            "application vector table"
        )

    if header.img_size == 0:
        raise ValueError("image declares a zero-length payload")

    if header.img_size % WRITE_GRANULARITY != 0:
        raise ValueError(
            f"payload size {header.img_size} is not a multiple of the "
            f"{WRITE_GRANULARITY}-byte write granularity"
        )

    payload = image[header.entry_offset:]

    if len(payload) != header.img_size:
        raise ValueError(
            f"file holds {len(payload)} payload bytes but the header "
            f"declares {header.img_size}"
        )

    if (zlib.crc32(payload) & 0xFFFFFFFF) != header.img_crc32:
        raise ValueError("payload CRC mismatch")

    if slot_size is not None:
        required = header.entry_offset + header.img_size

        if required > slot_size:
            raise ValueError(
                f"image needs {required} bytes but a slot holds {slot_size}"
            )

    return header
