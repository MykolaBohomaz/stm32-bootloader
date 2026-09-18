"""Bootloader wire protocol: constants and frame encoding.

Mirrors core/include/bl_proto.h. The two definitions are independent
implementations of one format, so tools/check_sync.py compares them in
CI and fails if they drift apart.

Frame layout:

    SOF (0x7E) | CMD | LEN (2, LE) | PAYLOAD (0..512) | CRC32 (4, LE)

The CRC covers CMD, LEN and PAYLOAD. SOF is a resynchronisation marker
and is not included.
"""

import struct
import zlib

SOF = 0x7E
PROTO_VERSION = 1
MAX_PAYLOAD = 512
FRAME_OVERHEAD = 8
MIN_RESPONSE_SIZE = 29

# A WRITE payload carries a four-byte destination offset ahead of its
# data, so a single request cannot transfer a full frame payload of
# firmware. The remainder is rounded down to whole write granules.
MAX_WRITE_DATA = ((MAX_PAYLOAD - 4) // 8) * 8

SLOT_A = 0
SLOT_B = 1
SLOT_COUNT = 2

# Reported when the device holds no boot metadata. 0xFF is the
# erased-flash value and is not a valid slot or state.
SLOT_NONE = 0xFF
BOOT_STATE_NONE = 0xFF

BOOT_CONFIRMED = 0x01
BOOT_TRIAL = 0x02

# Services the bootloader publishes to the running application, at a
# fixed address in its own flash region. Mirrors core/include/bl_api.h.
API_MAGIC = 0x49504142
API_VERSION = 1
API_ADDRESS = 0x08000200

# Boots a trial image is granted before the bootloader reverts.
MAX_BOOT_ATTEMPTS = 3

CMD_HELLO = 0x01
CMD_ERASE_SLOT = 0x02
CMD_WRITE = 0x03
CMD_VERIFY = 0x04
CMD_SET_ACTIVE = 0x05
CMD_RESET = 0x06

OK = 0x00

ERR_MAGIC = 0x10
ERR_HDR_VERSION = 0x11
ERR_HDR_CRC = 0x12

ERR_IMAGE_SIZE = 0x20
ERR_IMAGE_CRC = 0x21
ERR_ENTRY_OFFSET = 0x22
ERR_NO_VALID_IMAGE = 0x23
ERR_JUMP_REFUSED = 0x24
ERR_NO_METADATA = 0x25

ERR_FLASH_UNLOCK = 0x30
ERR_FLASH_ERASE = 0x31
ERR_FLASH_PROGRAM = 0x32
ERR_FLASH_VERIFY = 0x33
ERR_NOT_ERASED = 0x34

ERR_TIMEOUT = 0x40
ERR_INVALID_PACKET = 0x41
ERR_CRC = 0x42

ERR_NULL_POINTER = 0x50
ERR_INVALID_ARGUMENT = 0x51
ERR_UNKNOWN = 0x52
ERR_NOT_SUPPORTED = 0x53

# Status codes are reported to the operator by name; a bare number is
# not actionable.
STATUS_NAMES = {
    OK: "BL_OK",
    ERR_MAGIC: "BL_ERR_MAGIC",
    ERR_HDR_VERSION: "BL_ERR_HDR_VERSION",
    ERR_HDR_CRC: "BL_ERR_HDR_CRC",
    ERR_IMAGE_SIZE: "BL_ERR_IMAGE_SIZE",
    ERR_IMAGE_CRC: "BL_ERR_IMAGE_CRC",
    ERR_ENTRY_OFFSET: "BL_ERR_ENTRY_OFFSET",
    ERR_NO_VALID_IMAGE: "BL_ERR_NO_VALID_IMAGE",
    ERR_JUMP_REFUSED: "BL_ERR_JUMP_REFUSED",
    ERR_NO_METADATA: "BL_ERR_NO_METADATA",
    ERR_FLASH_UNLOCK: "BL_ERR_FLASH_UNLOCK",
    ERR_FLASH_ERASE: "BL_ERR_FLASH_ERASE",
    ERR_FLASH_PROGRAM: "BL_ERR_FLASH_PROGRAM",
    ERR_FLASH_VERIFY: "BL_ERR_FLASH_VERIFY",
    ERR_NOT_ERASED: "BL_ERR_NOT_ERASED",
    ERR_TIMEOUT: "BL_ERR_TIMEOUT",
    ERR_INVALID_PACKET: "BL_ERR_INVALID_PACKET",
    ERR_CRC: "BL_ERR_CRC",
    ERR_NULL_POINTER: "BL_ERR_NULL_POINTER",
    ERR_INVALID_ARGUMENT: "BL_ERR_INVALID_ARGUMENT",
    ERR_UNKNOWN: "BL_ERR_UNKNOWN",
    ERR_NOT_SUPPORTED: "BL_ERR_NOT_SUPPORTED",
}

COMMAND_NAMES = {
    CMD_HELLO: "HELLO",
    CMD_ERASE_SLOT: "ERASE_SLOT",
    CMD_WRITE: "WRITE",
    CMD_VERIFY: "VERIFY",
    CMD_SET_ACTIVE: "SET_ACTIVE",
    CMD_RESET: "RESET",
}

SLOT_NAMES = {SLOT_A: "A", SLOT_B: "B"}

BOOT_STATE_NAMES = {
    BOOT_CONFIRMED: "confirmed",
    BOOT_TRIAL: "trial",
    BOOT_STATE_NONE: "none",
}


def status_name(status):
    return STATUS_NAMES.get(status, f"unknown status 0x{status:02X}")


def slot_name(slot):
    return SLOT_NAMES.get(slot, "none" if slot == SLOT_NONE else str(slot))


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def encode_frame(cmd, payload=b""):
    """Encode a complete frame.

    Raises ValueError if the payload exceeds the protocol limit, rather
    than emitting a frame the device would reject.
    """
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(
            f"payload {len(payload)} exceeds the {MAX_PAYLOAD}-byte limit"
        )

    body = struct.pack("<BH", cmd, len(payload)) + payload

    return bytes([SOF]) + body + struct.pack("<I", crc32(body))


def decode_frame(frame):
    """Decode a complete frame into (cmd, payload).

    Raises ValueError on a malformed or corrupt frame.
    """
    if len(frame) < FRAME_OVERHEAD:
        raise ValueError("frame shorter than the fixed overhead")

    if frame[0] != SOF:
        raise ValueError(f"expected SOF 0x{SOF:02X}, got 0x{frame[0]:02X}")

    cmd, length = struct.unpack("<BH", frame[1:4])

    if len(frame) != FRAME_OVERHEAD + length:
        raise ValueError(
            f"frame length {len(frame)} does not match payload length {length}"
        )

    payload = frame[4:4 + length]
    (received,) = struct.unpack("<I", frame[4 + length:])
    expected = crc32(frame[1:4 + length])

    if received != expected:
        raise ValueError(
            f"CRC mismatch: got 0x{received:08X}, expected 0x{expected:08X}"
        )

    return cmd, payload
