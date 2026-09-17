"""Simulated device, presented as a serial port.

Loads the bootloader core and the simulated flash port as a shared
library and exposes the read/write interface tools/blflash.py expects.
This lets the host tool be driven against the real firmware logic with
no hardware attached.

The library is built by the host CMake configuration as `bl_sim`. Set
BL_SIM_LIB to its path, or leave it unset to search the usual build
directory.
"""

import ctypes
import os
import sys
from pathlib import Path

_RESPONSE_CAPACITY = 1024

_LIB_NAMES = ("libbl_sim.so", "libbl_sim.dylib", "bl_sim.dll")


class SimUnavailable(RuntimeError):
    """The simulated device library could not be located or loaded."""


def _candidate_paths():
    override = os.environ.get("BL_SIM_LIB")

    if override:
        yield Path(override)
        return

    root = Path(__file__).resolve().parent.parent

    for directory in (root / "build", root):
        for name in _LIB_NAMES:
            yield directory / name


def _load_library():
    tried = []

    for path in _candidate_paths():
        tried.append(str(path))

        if path.exists():
            return ctypes.CDLL(str(path))

    raise SimUnavailable(
        "could not find the simulated device library; build the host "
        "configuration with CMake, or set BL_SIM_LIB. Tried: "
        + ", ".join(tried)
    )


class SimulatedDevice:
    """A device implemented by the real bootloader core.

    Presents `write`, `read` and `reset_input_buffer` so it can stand in
    for a pyserial handle. Responses are produced synchronously when a
    complete request frame has been written, then handed out by `read`.
    """

    def __init__(self):
        self._lib = _load_library()
        self._bind()
        self._rx = bytearray()
        self._lib.sim_init()

    def _bind(self):
        lib = self._lib

        lib.sim_init.restype = None
        lib.sim_init.argtypes = []

        lib.sim_reboot.restype = None
        lib.sim_reboot.argtypes = []

        lib.sim_feed.restype = ctypes.c_uint32
        lib.sim_feed.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_uint32,
        ]

        lib.sim_boot.restype = ctypes.c_uint32
        lib.sim_boot.argtypes = []

        lib.sim_confirm.restype = ctypes.c_uint32
        lib.sim_confirm.argtypes = []

        lib.sim_reset_requested.restype = ctypes.c_uint32
        lib.sim_reset_requested.argtypes = []

        lib.sim_meta.restype = ctypes.c_uint32
        lib.sim_meta.argtypes = [
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
        ]

        lib.sim_slot_base.restype = ctypes.c_uint32
        lib.sim_slot_base.argtypes = [ctypes.c_uint32]

        lib.sim_slot_size.restype = ctypes.c_uint32
        lib.sim_slot_size.argtypes = [ctypes.c_uint32]

        lib.sim_corrupt.restype = None
        lib.sim_corrupt.argtypes = [ctypes.c_uint32, ctypes.c_uint32]

        lib.sim_fail_after_n_writes.restype = None
        lib.sim_fail_after_n_writes.argtypes = [ctypes.c_uint32]

        lib.sim_clear_failures.restype = None
        lib.sim_clear_failures.argtypes = []

    # -- serial-port interface -------------------------------------------

    def write(self, data):
        buffer = (ctypes.c_uint8 * len(data))(*data)
        response = (ctypes.c_uint8 * _RESPONSE_CAPACITY)()

        produced = self._lib.sim_feed(
            buffer, len(data), response, _RESPONSE_CAPACITY
        )

        if produced:
            self._rx.extend(bytes(response[:produced]))

        return len(data)

    def read(self, count=1):
        taken = self._rx[:count]
        del self._rx[:count]

        return bytes(taken)

    def reset_input_buffer(self):
        self._rx.clear()

    def close(self):
        pass

    # -- device control --------------------------------------------------

    def factory_reset(self):
        """Erase flash and discard all state."""
        self._rx.clear()
        self._lib.sim_init()

    def reboot(self):
        """Power-cycle the device: flash persists, state does not."""
        self._rx.clear()
        self._lib.sim_reboot()

    def boot(self):
        """Run the boot path; returns the jump target, or 0 if none."""
        return int(self._lib.sim_boot())

    def confirm(self):
        """Confirm the running image, as the application would."""
        return int(self._lib.sim_confirm())

    def reset_requested(self):
        return bool(self._lib.sim_reset_requested())

    def metadata(self):
        """Return (active_slot, state, boot_attempts), or None if absent."""
        active = ctypes.c_uint32(0)
        state = ctypes.c_uint32(0)
        attempts = ctypes.c_uint32(0)

        result = self._lib.sim_meta(
            ctypes.byref(active), ctypes.byref(state), ctypes.byref(attempts)
        )

        if result != 0:
            return None

        return (int(active.value), int(state.value), int(attempts.value))

    def slot_base(self, slot):
        return int(self._lib.sim_slot_base(slot))

    def slot_size(self, slot):
        return int(self._lib.sim_slot_size(slot))

    def corrupt(self, addr, length):
        self._lib.sim_corrupt(addr, length)

    def fail_after_n_writes(self, n):
        self._lib.sim_fail_after_n_writes(n)

    def clear_failures(self):
        self._lib.sim_clear_failures()


def available():
    """Whether the simulated device library can be loaded."""
    try:
        _load_library()
    except (SimUnavailable, OSError):
        return False

    return True


def main():
    """Report the simulated device's geometry, as a smoke test."""
    try:
        device = SimulatedDevice()
    except (SimUnavailable, OSError) as error:
        print(error, file=sys.stderr)
        return 1

    for slot in (0, 1):
        print(
            f"slot {slot}: base 0x{device.slot_base(slot):08X} "
            f"size {device.slot_size(slot)}"
        )

    print("metadata:", device.metadata())

    return 0


if __name__ == "__main__":
    sys.exit(main())
