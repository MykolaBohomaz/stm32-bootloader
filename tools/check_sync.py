#!/usr/bin/env python3
"""Check that the Python protocol definitions match the C headers.

The wire protocol and image format have two independent
implementations: the device firmware in core/ and the host tooling in
tools/. Nothing in either build would notice if a constant changed on
one side only, and the resulting failure would appear as a corrupt
image or an unintelligible response rather than as a mismatch.

This script parses the constants out of the C headers and compares them
against the Python modules. It is registered as a CTest case.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import blimage
import blproto

# Python name -> C name, for simple #define constants.
DEFINE_MAP = {
    "blproto.SOF": "BL_SOF",
    "blproto.PROTO_VERSION": "BL_PROTO_VERSION",
    "blproto.MAX_PAYLOAD": "BL_MAX_PAYLOAD",
    "blproto.MAX_WRITE_DATA": "BL_MAX_WRITE_DATA",
    "blproto.MIN_RESPONSE_SIZE": "BL_MIN_RESPONSE_SIZE",
    "blproto.SLOT_A": "BL_SLOT_A",
    "blproto.SLOT_B": "BL_SLOT_B",
    "blproto.SLOT_COUNT": "BL_SLOT_COUNT",
    "blproto.SLOT_NONE": "BL_SLOT_NONE",
    "blproto.BOOT_STATE_NONE": "BL_BOOT_STATE_NONE",
    "blimage.IMG_MAGIC": "BL_IMG_MAGIC",
    "blimage.HDR_VERSION": "BL_IMG_HDR_VERSION",
    "blimage.HDR_REGION_SIZE": "BL_IMG_HDR_REGION",
    "blimage.VTOR_MIN_ALIGNMENT": "BL_VTOR_MIN_ALIGNMENT",
}

# Python name -> C enumerator, for enum members.
ENUM_MAP = {
    "blproto.CMD_HELLO": "BL_CMD_HELLO",
    "blproto.CMD_ERASE_SLOT": "BL_CMD_ERASE_SLOT",
    "blproto.CMD_WRITE": "BL_CMD_WRITE",
    "blproto.CMD_VERIFY": "BL_CMD_VERIFY",
    "blproto.CMD_SET_ACTIVE": "BL_CMD_SET_ACTIVE",
    "blproto.CMD_RESET": "BL_CMD_RESET",
    "blproto.OK": "BL_OK",
    "blproto.ERR_MAGIC": "BL_ERR_MAGIC",
    "blproto.ERR_HDR_VERSION": "BL_ERR_HDR_VERSION",
    "blproto.ERR_HDR_CRC": "BL_ERR_HDR_CRC",
    "blproto.ERR_IMAGE_SIZE": "BL_ERR_IMAGE_SIZE",
    "blproto.ERR_IMAGE_CRC": "BL_ERR_IMAGE_CRC",
    "blproto.ERR_ENTRY_OFFSET": "BL_ERR_ENTRY_OFFSET",
    "blproto.ERR_NO_VALID_IMAGE": "BL_ERR_NO_VALID_IMAGE",
    "blproto.ERR_JUMP_REFUSED": "BL_ERR_JUMP_REFUSED",
    "blproto.ERR_NO_METADATA": "BL_ERR_NO_METADATA",
    "blproto.ERR_FLASH_UNLOCK": "BL_ERR_FLASH_UNLOCK",
    "blproto.ERR_FLASH_ERASE": "BL_ERR_FLASH_ERASE",
    "blproto.ERR_FLASH_PROGRAM": "BL_ERR_FLASH_PROGRAM",
    "blproto.ERR_FLASH_VERIFY": "BL_ERR_FLASH_VERIFY",
    "blproto.ERR_NOT_ERASED": "BL_ERR_NOT_ERASED",
    "blproto.ERR_TIMEOUT": "BL_ERR_TIMEOUT",
    "blproto.ERR_INVALID_PACKET": "BL_ERR_INVALID_PACKET",
    "blproto.ERR_CRC": "BL_ERR_CRC",
    "blproto.ERR_NULL_POINTER": "BL_ERR_NULL_POINTER",
    "blproto.ERR_INVALID_ARGUMENT": "BL_ERR_INVALID_ARGUMENT",
    "blproto.ERR_UNKNOWN": "BL_ERR_UNKNOWN",
    "blproto.ERR_NOT_SUPPORTED": "BL_ERR_NOT_SUPPORTED",
    "blproto.BOOT_CONFIRMED": "BL_BOOT_CONFIRMED",
    "blproto.BOOT_TRIAL": "BL_BOOT_TRIAL",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)

    return re.sub(r"//[^\n]*", "", text)


def parse_defines(text):
    """Collect integer #define values, evaluating simple expressions."""
    values = {}

    pattern = re.compile(
        r"^\s*#define\s+(BL_[A-Z0-9_]+)\s+(.+?)\s*$", re.MULTILINE
    )

    for name, raw in pattern.findall(text):
        expression = raw.strip()

        # Drop the unsigned suffixes C requires and Python rejects.
        expression = re.sub(r"\b(\d+)[uU][lL]{0,2}\b", r"\1", expression)
        expression = re.sub(r"\b(0[xX][0-9a-fA-F]+)[uU][lL]{0,2}\b",
                            r"\1", expression)

        # Resolve references to constants already parsed.
        for known, value in values.items():
            expression = re.sub(rf"\b{known}\b", str(value), expression)

        if not re.fullmatch(r"[0-9xXa-fA-F\s()+\-*/&|<>~]+", expression):
            continue

        # C integer division truncates; Python's '/' would produce a
        # float and silently disagree with the value the compiler sees.
        expression = expression.replace("/", "//").replace("////", "//")

        try:
            values[name] = int(eval(expression, {"__builtins__": {}}, {}))
        except (SyntaxError, TypeError, ValueError, ZeroDivisionError):
            continue

    return values


def parse_enumerators(text):
    """Collect enumerator values, following C's implicit increments."""
    values = {}

    for body in re.findall(r"typedef\s+enum\s*\{(.*?)\}", text, re.DOTALL):
        next_value = 0

        for entry in body.split(","):
            entry = entry.strip()

            if not entry:
                continue

            match = re.match(
                r"^(BL_[A-Z0-9_]+)\s*(?:=\s*(0[xX][0-9a-fA-F]+|\d+))?$", entry
            )

            if not match:
                continue

            name, raw = match.groups()

            if raw is not None:
                next_value = int(raw, 0)

            values[name] = next_value
            next_value += 1

    return values


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    include = root / "core" / "include"

    sources = [include / "bl_proto.h", include / "bl_meta.h",
               include / "bl_core.h"]

    text = ""

    for source in sources:
        if not source.exists():
            print(f"error: {source} not found", file=sys.stderr)
            return 2

        text += strip_comments(source.read_text()) + "\n"

    defines = parse_defines(text)
    enumerators = parse_enumerators(text)

    failures = []

    for python_name, c_name in {**DEFINE_MAP, **ENUM_MAP}.items():
        module, attribute = python_name.split(".")
        python_value = getattr(
            {"blproto": blproto, "blimage": blimage}[module], attribute
        )

        c_value = defines.get(c_name, enumerators.get(c_name))

        if c_value is None:
            failures.append(f"{c_name} not found in the C headers")
        elif c_value != python_value:
            failures.append(
                f"{python_name} is {python_value} but {c_name} is {c_value}"
            )

    # The header layout is a struct, not a constant, so it is checked by
    # size rather than by value.
    packed = blimage.build_header(0x1000, 0x2000, 0x30000, 512)[0]

    if len(packed) != blimage.HDR_STRUCT_SIZE:
        failures.append(
            f"packed header is {len(packed)} bytes, expected "
            f"{blimage.HDR_STRUCT_SIZE}"
        )

    if defines.get("BL_IMG_HDR_REGION", 0) < blimage.HDR_STRUCT_SIZE:
        failures.append("BL_IMG_HDR_REGION cannot hold the image header")

    if failures:
        for failure in failures:
            print(f"error: {failure}", file=sys.stderr)

        return 1

    checked = len(DEFINE_MAP) + len(ENUM_MAP)
    print(f"{checked} constants match between the C headers and Python")

    return 0


if __name__ == "__main__":
    sys.exit(main())
