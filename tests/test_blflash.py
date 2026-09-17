"""Tests for the host tooling.

The transport and protocol layers are tested against a fake port that
can drop responses and corrupt frames — conditions a real link produces
but which are impossible to trigger on demand.

The command and update layers are tested against the real bootloader
core, loaded as a shared library. The protocol has no external
specification, so exercising the actual firmware logic is the only way
to establish that the two implementations agree.
"""

import os
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import blflash
import blimage
import blproto
import blsim


PAYLOAD_SIZE = 4096
INITIAL_MSP = 0x20010000


def build_image(app_base, fw_version, payload_size=PAYLOAD_SIZE):
    """Build a complete .blimg whose payload is a plausible image.

    The payload opens with a vector table the device will accept as an
    entry point, so the same image can be used for boot tests.
    """
    payload = bytearray(
        (i * 7 + 3) & 0xFF for i in range(payload_size)
    )

    reset_vector = (app_base + 0x100) | 1

    payload[0:4] = INITIAL_MSP.to_bytes(4, "little")
    payload[4:8] = reset_vector.to_bytes(4, "little")

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


class FakePort:
    """A port whose behaviour a test can dictate.

    Answers requests from a table, and can be told to drop responses or
    to corrupt the bytes it returns.
    """

    def __init__(self, responder):
        self._responder = responder
        self._rx = bytearray()
        self._tx = bytearray()
        self.drop_next = 0
        self.corrupt_next = 0
        self.requests = []
        self.timeout = 0.0

    def write(self, data):
        self._tx.extend(data)

        # Requests are complete frames in these tests.
        cmd, payload = blproto.decode_frame(bytes(self._tx))
        self._tx.clear()
        self.requests.append((cmd, payload))

        response = self._responder(cmd, payload)

        if self.drop_next > 0:
            self.drop_next -= 1
            return len(data)

        if self.corrupt_next > 0:
            self.corrupt_next -= 1
            response = bytearray(response)
            response[-1] ^= 0xFF
            response = bytes(response)

        self._rx.extend(response)

        return len(data)

    def read(self, count=1):
        taken = self._rx[:count]
        del self._rx[:count]

        return bytes(taken)

    def reset_input_buffer(self):
        self._rx.clear()

    def close(self):
        pass


def status_response(cmd, status, data=b""):
    return blproto.encode_frame(cmd, bytes([status]) + data)


class FrameCodecTest(unittest.TestCase):
    def test_round_trip(self):
        payload = bytes(range(64))
        cmd, decoded = blproto.decode_frame(
            blproto.encode_frame(blproto.CMD_WRITE, payload)
        )

        self.assertEqual(blproto.CMD_WRITE, cmd)
        self.assertEqual(payload, decoded)

    def test_empty_payload(self):
        frame = blproto.encode_frame(blproto.CMD_HELLO)

        self.assertEqual(blproto.FRAME_OVERHEAD, len(frame))
        self.assertEqual((blproto.CMD_HELLO, b""),
                         blproto.decode_frame(frame))

    def test_maximum_payload(self):
        payload = bytes(blproto.MAX_PAYLOAD)
        frame = blproto.encode_frame(blproto.CMD_WRITE, payload)

        self.assertEqual(blproto.MAX_PAYLOAD + blproto.FRAME_OVERHEAD,
                         len(frame))

    def test_oversized_payload_is_refused(self):
        with self.assertRaises(ValueError):
            blproto.encode_frame(blproto.CMD_WRITE,
                                 bytes(blproto.MAX_PAYLOAD + 1))

    def test_corrupt_payload_is_detected(self):
        frame = bytearray(
            blproto.encode_frame(blproto.CMD_WRITE, b"\x01\x02\x03\x04")
        )
        frame[5] ^= 0xFF

        with self.assertRaises(ValueError):
            blproto.decode_frame(bytes(frame))

    def test_missing_sof_is_detected(self):
        frame = bytearray(blproto.encode_frame(blproto.CMD_HELLO))
        frame[0] = 0x00

        with self.assertRaises(ValueError):
            blproto.decode_frame(bytes(frame))

    def test_write_chunk_fits_a_frame(self):
        """A chunk of the advertised maximum must be encodable."""
        payload = b"\x00\x00\x00\x00" + bytes(blproto.MAX_WRITE_DATA)

        self.assertLessEqual(len(payload), blproto.MAX_PAYLOAD)
        blproto.encode_frame(blproto.CMD_WRITE, payload)


class TransportTest(unittest.TestCase):
    """Retry and resynchronisation behaviour, driven by a fake port."""

    def test_a_dropped_response_is_retried(self):
        port = FakePort(
            lambda cmd, payload: status_response(cmd, blproto.OK)
        )
        port.drop_next = 1

        bootloader = blflash.Bootloader(port)
        bootloader.reset()

        self.assertEqual(2, len(port.requests))

    def test_a_corrupt_response_is_retried(self):
        port = FakePort(
            lambda cmd, payload: status_response(cmd, blproto.OK)
        )
        port.corrupt_next = 1

        bootloader = blflash.Bootloader(port)
        bootloader.reset()

        self.assertEqual(2, len(port.requests))

    def test_persistent_silence_raises(self):
        port = FakePort(
            lambda cmd, payload: status_response(cmd, blproto.OK)
        )
        port.drop_next = 99

        bootloader = blflash.Bootloader(port, retries=3)

        with self.assertRaises(blflash.BootloaderTimeout):
            bootloader.reset()

        self.assertEqual(3, len(port.requests))

    def test_a_response_for_another_command_is_discarded(self):
        """A late response must not be read as the answer to a new request."""
        def responder(cmd, payload):
            stale = status_response(blproto.CMD_HELLO, blproto.OK, bytes(20))

            return stale + status_response(cmd, blproto.OK)

        port = FakePort(responder)
        bootloader = blflash.Bootloader(port)

        bootloader.reset()

        self.assertEqual(1, len(port.requests))

    def test_an_error_status_is_raised_with_its_name(self):
        port = FakePort(
            lambda cmd, payload: status_response(cmd, blproto.ERR_IMAGE_CRC)
        )

        bootloader = blflash.Bootloader(port)

        with self.assertRaises(blflash.DeviceError) as caught:
            bootloader.verify(blproto.SLOT_A)

        self.assertIn("BL_ERR_IMAGE_CRC", str(caught.exception))


class WriteIdempotencyTest(unittest.TestCase):
    """The one place the host must interpret an error as success."""

    def test_a_lost_acknowledgement_is_recovered(self):
        """A retry refused because the data already landed is success.

        Had the first attempt not landed, the region would still be
        erased and the retry would have succeeded, so on a retry this
        status can only mean the write is already in flash.
        """
        state = {"writes": 0}

        def responder(cmd, payload):
            if cmd != blproto.CMD_WRITE:
                return status_response(cmd, blproto.OK)

            state["writes"] += 1

            if state["writes"] == 1:
                return status_response(cmd, blproto.OK, payload[:4])

            return status_response(cmd, blproto.ERR_NOT_ERASED)

        port = FakePort(responder)
        port.drop_next = 1

        bootloader = blflash.Bootloader(port)

        # Must not raise: the first attempt was applied, its
        # acknowledgement was lost, and the retry reports the region as
        # already written.
        bootloader.write(512, bytes(8))

    def test_a_first_attempt_refusal_is_fatal(self):
        """On a first attempt the same status is a genuine error."""
        port = FakePort(
            lambda cmd, payload: status_response(cmd, blproto.ERR_NOT_ERASED)
        )

        bootloader = blflash.Bootloader(port)

        with self.assertRaises(blflash.DeviceError):
            bootloader.write(512, bytes(8))


class ImageValidationTest(unittest.TestCase):
    def setUp(self):
        self.image = build_image(0x08009200, 0x00010000)

    def test_a_valid_image_passes(self):
        header = blimage.validate(self.image)

        self.assertEqual(blimage.IMG_MAGIC, header.magic)
        self.assertEqual(PAYLOAD_SIZE, header.img_size)
        self.assertEqual(blimage.HDR_REGION_SIZE, header.entry_offset)

    def test_a_truncated_image_is_rejected(self):
        with self.assertRaises(ValueError):
            blimage.validate(self.image[:256])

    def test_a_corrupt_payload_is_rejected(self):
        damaged = bytearray(self.image)
        damaged[1000] ^= 0xFF

        with self.assertRaises(ValueError):
            blimage.validate(bytes(damaged))

    def test_a_corrupt_header_is_rejected(self):
        damaged = bytearray(self.image)
        damaged[6] ^= 0xFF

        with self.assertRaises(ValueError):
            blimage.validate(bytes(damaged))

    def test_an_oversized_image_is_rejected(self):
        """Rejecting locally matters: an erase cannot be undone."""
        with self.assertRaises(ValueError):
            blimage.validate(self.image, slot_size=1024)


@unittest.skipUnless(blsim.available(),
                     "the simulated device library is not built")
class SimulatedDeviceTest(unittest.TestCase):
    """End-to-end tests against the real bootloader core."""

    def setUp(self):
        self.device = blsim.SimulatedDevice()
        self.device.factory_reset()
        self.bootloader = blflash.Bootloader(self.device)

    def app_base(self, slot):
        return self.device.slot_base(slot) + blimage.HDR_REGION_SIZE

    def image_for(self, slot, fw_version):
        return build_image(self.app_base(slot), fw_version)

    def test_hello_reports_a_pristine_device(self):
        info = self.bootloader.hello()

        self.assertEqual(blproto.PROTO_VERSION, info.proto_version)
        self.assertEqual(blimage.HDR_VERSION, info.hdr_version)
        self.assertEqual(blproto.SLOT_COUNT, info.slot_count)
        self.assertEqual(blproto.SLOT_NONE, info.active_slot)
        self.assertEqual(blproto.BOOT_STATE_NONE, info.boot_state)
        self.assertEqual(blproto.MAX_WRITE_DATA, info.max_write)

    def test_an_update_installs_and_arms_an_image(self):
        image = self.image_for(blproto.SLOT_A, 0x00010000)

        slot = self.bootloader.update(image)

        self.assertEqual(blproto.SLOT_A, slot)

        stored = self.bootloader.verify(slot)
        header = blimage.decode_header(image)

        self.assertEqual(header.img_size, stored.img_size)
        self.assertEqual(header.img_crc32, stored.img_crc32)
        self.assertEqual(header.fw_version, stored.fw_version)

        self.assertEqual(
            (blproto.SLOT_A, blproto.BOOT_TRIAL, 0),
            self.device.metadata(),
        )

    def test_an_installed_image_boots(self):
        self.bootloader.update(self.image_for(blproto.SLOT_A, 0x00010000))

        self.device.reboot()

        self.assertEqual(self.app_base(blproto.SLOT_A), self.device.boot())

    def test_a_second_update_targets_the_other_slot(self):
        """The slot that would boot must not be erased."""
        self.bootloader.update(self.image_for(blproto.SLOT_A, 0x00010000))
        self.device.reboot()
        self.device.boot()
        self.device.confirm()

        second = self.bootloader.update(
            self.image_for(blproto.SLOT_B, 0x00020000)
        )

        self.assertEqual(blproto.SLOT_B, second)

    def test_the_active_slot_cannot_be_erased(self):
        self.bootloader.update(self.image_for(blproto.SLOT_A, 0x00010000))

        with self.assertRaises(blflash.BootloaderError):
            self.bootloader.update(
                self.image_for(blproto.SLOT_A, 0x00030000),
                slot=blproto.SLOT_A,
            )

    def test_an_unconfirmed_image_is_reverted(self):
        """The rollback path, driven entirely through the protocol."""
        self.bootloader.update(self.image_for(blproto.SLOT_A, 0x00010000))
        self.device.reboot()
        self.device.boot()
        self.device.confirm()

        self.bootloader.update(self.image_for(blproto.SLOT_B, 0x00020000))

        # The new image boots but never confirms itself.
        for _ in range(3):
            self.device.reboot()
            self.assertEqual(self.app_base(blproto.SLOT_B),
                             self.device.boot())

        # The next boot reverts to the previously confirmed image.
        self.device.reboot()

        self.assertEqual(self.app_base(blproto.SLOT_A), self.device.boot())

        active, state, _ = self.device.metadata()

        self.assertEqual(blproto.SLOT_A, active)
        self.assertEqual(blproto.BOOT_CONFIRMED, state)

    def test_a_confirmed_image_is_kept(self):
        self.bootloader.update(self.image_for(blproto.SLOT_A, 0x00010000))
        self.device.reboot()
        self.device.boot()
        self.device.confirm()

        for _ in range(6):
            self.device.reboot()
            self.assertEqual(self.app_base(blproto.SLOT_A),
                             self.device.boot())

    def test_verify_reports_the_reason_a_slot_is_unusable(self):
        status = self.bootloader.slot_status(blproto.SLOT_B)

        self.assertEqual(blproto.ERR_MAGIC, status)

    def test_a_corrupted_slot_is_detected(self):
        self.bootloader.update(self.image_for(blproto.SLOT_A, 0x00010000))

        self.device.corrupt(self.app_base(blproto.SLOT_A) + 64, 8)

        status = self.bootloader.slot_status(blproto.SLOT_A)

        self.assertEqual(blproto.ERR_IMAGE_CRC, status)

    def test_an_interrupted_update_leaves_the_slot_invalid(self):
        """Committing the header last is what makes this safe."""
        image = self.image_for(blproto.SLOT_A, 0x00010000)
        header = blimage.decode_header(image)
        payload = image[header.entry_offset:]

        self.bootloader.hello()
        self.bootloader.erase_slot(blproto.SLOT_A)

        # Write the payload but stop before committing the header.
        sent = 0

        while sent < len(payload):
            chunk = payload[sent:sent + blproto.MAX_WRITE_DATA]
            self.bootloader.write(header.entry_offset + sent, chunk)
            sent += len(chunk)

        self.assertEqual(
            blproto.ERR_MAGIC,
            self.bootloader.slot_status(blproto.SLOT_A),
        )

    def test_a_flash_failure_is_reported(self):
        image = self.image_for(blproto.SLOT_A, 0x00010000)

        self.bootloader.hello()
        self.bootloader.erase_slot(blproto.SLOT_A)
        self.device.fail_after_n_writes(0)

        with self.assertRaises(blflash.DeviceError) as caught:
            self.bootloader.write(blimage.HDR_REGION_SIZE, bytes(8))

        self.assertEqual(blproto.ERR_FLASH_PROGRAM, caught.exception.status)

        self.device.clear_failures()

    def test_reset_is_acknowledged_before_it_happens(self):
        self.bootloader.reset()

        self.assertTrue(self.device.reset_requested())

    def test_an_oversized_image_is_refused_before_erasing(self):
        info = self.bootloader.hello()

        oversized = build_image(
            self.app_base(blproto.SLOT_A),
            0x00010000,
            payload_size=info.slot_size,
        )

        with self.assertRaises(ValueError):
            self.bootloader.update(oversized)

        # Nothing was erased, so the device is untouched.
        self.assertEqual(
            blproto.ERR_MAGIC,
            self.bootloader.slot_status(blproto.SLOT_A),
        )


if __name__ == "__main__":
    unittest.main()
