"""Host tool for updating a device over the bootloader protocol.

Layered deliberately:

  transport   bytes in and out of a port, with deadlines
  protocol    frame encode/decode and one retrying transaction
  commands    one method per protocol command
  update      the ordered sequence that performs an update

Keeping them separate is what allows the upper layers to be tested
against a simulated device with no hardware attached.
"""

import argparse
import sys
import time
from collections import namedtuple
from pathlib import Path

import blimage
import blproto

# Per-command timeouts. Erasing a slot erases every page in it, which on
# an STM32L432 is about 22 ms per 2 KiB page, so a 108 KiB slot takes
# over a second. Every other command answers immediately.
DEFAULT_TIMEOUT = 1.0
ERASE_TIMEOUT = 10.0

DEFAULT_RETRIES = 3

Response = namedtuple("Response", "cmd status data attempts")

DeviceInfo = namedtuple(
    "DeviceInfo",
    "proto_version hdr_version slot_count slot_size write_granularity "
    "erase_granularity max_write active_slot boot_state boot_attempts",
)

SlotInfo = namedtuple("SlotInfo", "img_size img_crc32 fw_version")


class BootloaderError(Exception):
    """Base class for every failure this module reports."""


class BootloaderTimeout(BootloaderError):
    """The device did not answer within the allotted time."""


class ProtocolError(BootloaderError):
    """The device answered with something unintelligible."""


class DeviceError(BootloaderError):
    """The device answered and refused the request."""

    def __init__(self, cmd, status):
        self.cmd = cmd
        self.status = status

        super().__init__(
            f"{blproto.COMMAND_NAMES.get(cmd, f'command 0x{cmd:02X}')} "
            f"failed: {blproto.status_name(status)}"
        )


class Bootloader:
    """A bootloader reachable over a byte-oriented port.

    `port` must provide `write`, `read` and `reset_input_buffer`; both a
    pyserial handle and tools/blsim.SimulatedDevice satisfy this.
    """

    def __init__(self, port, retries=DEFAULT_RETRIES, log=None):
        self._port = port
        self._retries = retries
        self._log = log if log is not None else (lambda message: None)
        self._info = None

    # -- transport -------------------------------------------------------

    def _read_exactly(self, count, deadline):
        """Read exactly count bytes, or raise on running out of time.

        A port's read() returns *up to* the requested number of bytes,
        so accumulating explicitly is required; assuming otherwise works
        on a fast link and fails intermittently on a slow one.
        """
        buffer = bytearray()

        while len(buffer) < count:
            remaining = deadline - time.monotonic()

            if remaining <= 0:
                raise BootloaderTimeout(
                    f"wanted {count} bytes, received {len(buffer)}"
                )

            self._set_timeout(remaining)

            chunk = self._port.read(count - len(buffer))

            if chunk:
                buffer.extend(chunk)
            elif not hasattr(self._port, "timeout"):
                # A port without timeout support cannot block, so an
                # empty read means no data will arrive.
                raise BootloaderTimeout(
                    f"wanted {count} bytes, received {len(buffer)}"
                )

        return bytes(buffer)

    def _set_timeout(self, seconds):
        if hasattr(self._port, "timeout"):
            self._port.timeout = max(seconds, 0.0)

    def _read_frame(self, deadline):
        """Read one frame, discarding bytes until a start marker."""
        while True:
            byte = self._read_exactly(1, deadline)

            if byte[0] == blproto.SOF:
                break

        head = self._read_exactly(3, deadline)
        length = int.from_bytes(head[1:3], "little")

        if length > blproto.MAX_PAYLOAD:
            raise ProtocolError(
                f"declared payload {length} exceeds the protocol limit"
            )

        payload = self._read_exactly(length, deadline) if length else b""
        crc = self._read_exactly(4, deadline)

        return blproto.decode_frame(bytes([blproto.SOF]) + head + payload + crc)

    # -- protocol --------------------------------------------------------

    def _transact(self, cmd, payload=b"", timeout=DEFAULT_TIMEOUT):
        """Send a request and return the device's response.

        Transport failures are retried; a response carrying an error
        status is returned rather than raised, because only the calling
        command knows whether a given status is fatal.
        """
        request = blproto.encode_frame(cmd, payload)
        last_error = None

        for attempt in range(1, self._retries + 1):
            # Discard anything already buffered. A response that arrived
            # after a previous attempt timed out would otherwise be read
            # as the answer to this request, leaving every subsequent
            # exchange one response behind.
            self._port.reset_input_buffer()
            self._port.write(request)

            deadline = time.monotonic() + timeout

            try:
                while True:
                    response_cmd, data = self._read_frame(deadline)

                    if response_cmd == cmd:
                        break

                    # A frame for a different command is stale or noise.
                    self._log(
                        f"discarding response for command "
                        f"0x{response_cmd:02X} while awaiting 0x{cmd:02X}"
                    )
            except (BootloaderTimeout, ProtocolError, ValueError) as error:
                last_error = error
                self._log(f"attempt {attempt} failed: {error}")
                continue

            if not data:
                last_error = ProtocolError("response carried no status byte")
                continue

            return Response(response_cmd, data[0], data[1:], attempt)

        raise BootloaderTimeout(
            f"no usable response to "
            f"{blproto.COMMAND_NAMES.get(cmd, hex(cmd))} after "
            f"{self._retries} attempts: {last_error}"
        )

    def _expect_ok(self, cmd, payload=b"", timeout=DEFAULT_TIMEOUT):
        response = self._transact(cmd, payload, timeout)

        if response.status != blproto.OK:
            raise DeviceError(cmd, response.status)

        return response.data

    # -- commands --------------------------------------------------------

    def hello(self):
        data = self._expect_ok(blproto.CMD_HELLO)

        if len(data) < 20:
            raise ProtocolError(
                f"HELLO returned {len(data)} bytes, expected at least 20"
            )

        info = DeviceInfo(
            proto_version=data[0],
            hdr_version=data[1],
            slot_count=data[2],
            slot_size=int.from_bytes(data[3:7], "little"),
            write_granularity=int.from_bytes(data[7:11], "little"),
            erase_granularity=int.from_bytes(data[11:15], "little"),
            max_write=int.from_bytes(data[15:17], "little"),
            active_slot=data[17],
            boot_state=data[18],
            boot_attempts=data[19],
        )

        if info.proto_version != blproto.PROTO_VERSION:
            raise ProtocolError(
                f"device speaks protocol version {info.proto_version}, "
                f"this tool speaks {blproto.PROTO_VERSION}"
            )

        if info.hdr_version != blimage.HDR_VERSION:
            raise ProtocolError(
                f"device expects image header version {info.hdr_version}, "
                f"this tool produces {blimage.HDR_VERSION}"
            )

        self._info = info

        return info

    def erase_slot(self, slot):
        self._expect_ok(
            blproto.CMD_ERASE_SLOT, bytes([slot]), timeout=ERASE_TIMEOUT
        )

    def write(self, offset, data):
        payload = offset.to_bytes(4, "little") + data
        response = self._transact(blproto.CMD_WRITE, payload)

        if response.status == blproto.OK:
            echoed = int.from_bytes(response.data[:4], "little")

            if echoed != offset:
                raise ProtocolError(
                    f"device acknowledged offset {echoed}, expected {offset}"
                )

            return

        # A retried write may be refused because the first attempt
        # already landed and only its acknowledgement was lost. Had the
        # write not landed, the region would still be erased and the
        # retry would have succeeded, so on a retry this status can only
        # mean the data is already in flash.
        if (
            response.status == blproto.ERR_NOT_ERASED
            and response.attempts > 1
        ):
            self._log(f"offset {offset} was already written; continuing")
            return

        raise DeviceError(blproto.CMD_WRITE, response.status)

    def verify(self, slot):
        data = self._expect_ok(blproto.CMD_VERIFY, bytes([slot]))

        if len(data) < 12:
            raise ProtocolError(
                f"VERIFY returned {len(data)} bytes, expected at least 12"
            )

        return SlotInfo(
            img_size=int.from_bytes(data[0:4], "little"),
            img_crc32=int.from_bytes(data[4:8], "little"),
            fw_version=int.from_bytes(data[8:12], "little"),
        )

    def slot_status(self, slot):
        """Return SlotInfo for a slot, or the status code refusing it."""
        response = self._transact(blproto.CMD_VERIFY, bytes([slot]))

        if response.status != blproto.OK:
            return response.status

        return SlotInfo(
            img_size=int.from_bytes(response.data[0:4], "little"),
            img_crc32=int.from_bytes(response.data[4:8], "little"),
            fw_version=int.from_bytes(response.data[8:12], "little"),
        )

    def set_active(self, slot):
        self._expect_ok(blproto.CMD_SET_ACTIVE, bytes([slot]))

    def reset(self):
        self._expect_ok(blproto.CMD_RESET)

    # -- orchestration ---------------------------------------------------

    def choose_target_slot(self, info):
        """Pick the slot to overwrite.

        The slot that would currently boot must be preserved: erasing it
        removes the only fallback, so an interrupted update would leave
        the device with nothing to run.
        """
        if info.active_slot != blproto.SLOT_NONE:
            return (info.active_slot + 1) % info.slot_count

        # No metadata, so no slot is nominated and the bootloader would
        # select by firmware version. Overwrite the lower version, which
        # is the one it would not choose.
        statuses = {
            slot: self.slot_status(slot) for slot in range(info.slot_count)
        }

        valid = {
            slot: status
            for slot, status in statuses.items()
            if isinstance(status, SlotInfo)
        }

        if not valid:
            return blproto.SLOT_A

        if len(valid) == 1:
            occupied = next(iter(valid))
            return (occupied + 1) % info.slot_count

        return min(valid, key=lambda slot: valid[slot].fw_version)

    def update(self, image, slot=None, progress=None):
        """Install an image and arm it for a trial boot.

        Returns the slot that was written.
        """
        info = self.hello()

        # Validate before erasing. A rejected image costs nothing here
        # and costs a slot once the erase has happened.
        header = blimage.validate(image, slot_size=info.slot_size)

        if slot is None:
            slot = self.choose_target_slot(info)

        if slot >= info.slot_count:
            raise BootloaderError(
                f"slot {slot} does not exist on this device"
            )

        if slot == info.active_slot:
            raise BootloaderError(
                f"refusing to erase slot {blproto.slot_name(slot)}, which "
                "is the slot the device would currently boot"
            )

        self._log(f"erasing slot {blproto.slot_name(slot)}")
        self.erase_slot(slot)

        payload = image[header.entry_offset:]
        chunk_size = min(info.max_write, blproto.MAX_WRITE_DATA)

        if chunk_size % info.write_granularity != 0:
            raise ProtocolError(
                f"device reported an unusable write size {info.max_write}"
            )

        sent = 0

        while sent < len(payload):
            chunk = payload[sent:sent + chunk_size]
            self.write(header.entry_offset + sent, chunk)
            sent += len(chunk)

            if progress is not None:
                progress(sent, len(payload))

        # The header is committed last. An update interrupted before
        # this point leaves a slot with no valid header, which the
        # bootloader rejects cleanly, rather than a valid header
        # describing an incomplete payload.
        self._log("committing image header")
        self.write(0, image[:blimage.HDR_STRUCT_SIZE])

        self._log("verifying")
        stored = self.verify(slot)

        if stored.img_crc32 != header.img_crc32:
            raise BootloaderError(
                "device reports a different payload checksum than the "
                "image supplied"
            )

        self._log(f"arming slot {blproto.slot_name(slot)} for trial boot")
        self.set_active(slot)

        return slot


# -- command line --------------------------------------------------------


def _open_port(args):
    if args.sim:
        import blsim

        return blsim.SimulatedDevice()

    try:
        import serial
    except ImportError:
        raise BootloaderError(
            "pyserial is required for serial ports; install it with "
            "'pip install pyserial', or use --sim"
        )

    if not args.port:
        raise BootloaderError("--port is required unless --sim is given")

    return serial.Serial(args.port, args.baud, timeout=DEFAULT_TIMEOUT)


def _progress_printer(enabled):
    if not enabled:
        return None

    state = {"last": -1}

    def report(sent, total):
        percent = sent * 100 // total

        if percent != state["last"]:
            state["last"] = percent
            print(f"\r  writing {percent:3d}%", end="", flush=True)

            if sent == total:
                print()

    return report


def _cmd_status(bootloader, _args):
    info = bootloader.hello()

    print(f"protocol version     {info.proto_version}")
    print(f"image header version {info.hdr_version}")
    print(f"slots                {info.slot_count} x {info.slot_size} bytes")
    print(f"write granularity    {info.write_granularity}")
    print(f"erase granularity    {info.erase_granularity}")
    print(f"max write per frame  {info.max_write}")

    state = blproto.BOOT_STATE_NAMES.get(
        info.boot_state, f"0x{info.boot_state:02X}"
    )

    print(f"active slot          {blproto.slot_name(info.active_slot)}")
    print(f"boot state           {state}")

    if info.boot_state == blproto.BOOT_TRIAL:
        print(
            f"trial progress       attempt {info.boot_attempts} "
            f"(image is reverted once attempts are exhausted)"
        )

    for slot in range(info.slot_count):
        status = bootloader.slot_status(slot)

        if isinstance(status, SlotInfo):
            print(
                f"slot {blproto.slot_name(slot)}               "
                f"v{blimage.format_version(status.fw_version)}, "
                f"{status.img_size} bytes, "
                f"crc 0x{status.img_crc32:08X}"
            )
        else:
            print(
                f"slot {blproto.slot_name(slot)}               "
                f"{blproto.status_name(status)}"
            )

    return 0


def _cmd_flash(bootloader, args):
    image = Path(args.image).read_bytes()

    slot = None

    if args.slot is not None:
        slot = {"a": blproto.SLOT_A, "b": blproto.SLOT_B}[args.slot.lower()]

    written = bootloader.update(
        image, slot=slot, progress=_progress_printer(not args.quiet)
    )

    print(f"installed into slot {blproto.slot_name(written)}")

    if not args.no_reset:
        print("resetting")
        bootloader.reset()

    print(
        "the new image must confirm itself within "
        "its allotted boot attempts or the bootloader will revert"
    )

    return 0


def _cmd_verify(bootloader, args):
    slot = {"a": blproto.SLOT_A, "b": blproto.SLOT_B}[args.slot.lower()]
    info = bootloader.verify(slot)

    print(
        f"slot {blproto.slot_name(slot)}: "
        f"v{blimage.format_version(info.fw_version)}, "
        f"{info.img_size} bytes, crc 0x{info.img_crc32:08X}"
    )

    return 0


def _cmd_reset(bootloader, _args):
    bootloader.reset()
    print("reset requested")

    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Update a device over the bootloader protocol"
    )

    parser.add_argument("--port", help="serial port, e.g. /dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--sim",
        action="store_true",
        help="talk to a simulated device instead of a serial port",
    )
    parser.add_argument(
        "--retries", type=int, default=DEFAULT_RETRIES,
        help="transport retries per request",
    )
    parser.add_argument("--quiet", action="store_true")

    sub = parser.add_subparsers(dest="command", required=True)

    flash = sub.add_parser("flash", help="install an image")
    flash.add_argument("image", help="a .blimg file from mkimage.py")
    flash.add_argument("--slot", choices=("a", "b", "A", "B"))
    flash.add_argument(
        "--no-reset", action="store_true",
        help="leave the device in the bootloader after installing",
    )
    flash.set_defaults(handler=_cmd_flash)

    status = sub.add_parser("status", help="report device and slot state")
    status.set_defaults(handler=_cmd_status)

    verify = sub.add_parser("verify", help="validate a slot's image")
    verify.add_argument("--slot", required=True, choices=("a", "b", "A", "B"))
    verify.set_defaults(handler=_cmd_verify)

    reset = sub.add_parser("reset", help="restart the device")
    reset.set_defaults(handler=_cmd_reset)

    args = parser.parse_args(argv)

    def log(message):
        if not args.quiet:
            print(f"  {message}")

    try:
        port = _open_port(args)
    except BootloaderError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    bootloader = Bootloader(port, retries=args.retries, log=log)

    try:
        return args.handler(bootloader, args)
    except (BootloaderError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
