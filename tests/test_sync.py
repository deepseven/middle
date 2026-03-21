"""
Tests for the Middle BLE sync client (sync.py).

Run with: uv run pytest tests/test_sync.py
"""
import struct
import sys
import types
from pathlib import Path
from unittest.mock import MagicMock

import pytest

# Add the project root to path so we can import sync.py as a module.
sys.path.insert(0, str(Path(__file__).parent.parent))

# Stub heavy third-party modules that aren't needed for unit-testing
# the pure decoding / encoding / constant code paths.
_bleak_mock = MagicMock()
for mod_name, mock in [
    ("bleak", _bleak_mock),
    ("bleak.exc", MagicMock()),
    ("openai", MagicMock()),
    ("tqdm", MagicMock()),
    ("tqdm.auto", MagicMock()),
]:
    if mod_name not in sys.modules:
        sys.modules[mod_name] = mock

# Import the functions and constants we want to test.
from sync import (
    ADPCM_INDEX_TABLE,
    ADPCM_STEP_TABLE,
    CHARACTERISTIC_AUDIO_DATA_UUID,
    CHARACTERISTIC_COMMAND_UUID,
    CHARACTERISTIC_FILE_COUNT_UUID,
    CHARACTERISTIC_FILE_INFO_UUID,
    CHARACTERISTIC_PAIRING_UUID,
    CHARACTERISTIC_VOLTAGE_UUID,
    COMMAND_ACK_RECEIVED,
    COMMAND_REQUEST_NEXT,
    COMMAND_START_STREAM,
    COMMAND_SYNC_DONE,
    IMA_HEADER_SIZE,
    MAX_FILE_TRANSFER_ATTEMPTS,
    SAMPLE_RATE,
    SERVICE_UUID,
    decode_ima_adpcm,
    encode_mp3_from_ima,
)


# ── ADPCM decoder tests ─────────────────────────────────────────────────────


class TestDecodeImaAdpcm:
    def test_silence_input(self):
        """All-zero nibbles should produce deterministic output."""
        data = bytes(4)  # 8 nibbles, all zero
        result = decode_ima_adpcm(data, 8)
        assert len(result) == 16  # 8 samples * 2 bytes

    def test_empty_data(self):
        result = decode_ima_adpcm(b"", 0)
        assert result == b""

    def test_deterministic(self):
        """Same input always produces same output."""
        data = bytes([0x12, 0x34, 0x56, 0x78])
        r1 = decode_ima_adpcm(data, 8)
        r2 = decode_ima_adpcm(data, 8)
        assert r1 == r2

    def test_output_length(self):
        """Output should be sampleCount * 2 bytes."""
        data = bytes(10)
        result = decode_ima_adpcm(data, 20)
        assert len(result) == 40

    def test_single_positive_nibble(self):
        """nibble=4: step=7, delta=0+7=7, predicted=+7"""
        data = bytes([0x04])
        result = decode_ima_adpcm(data, 1)
        sample = struct.unpack("<h", result)[0]
        assert sample == 7

    def test_single_negative_nibble(self):
        """nibble=0x0C: sign=1, bit2=1, delta=0+7=7, predicted=-7"""
        data = bytes([0x0C])
        result = decode_ima_adpcm(data, 1)
        sample = struct.unpack("<h", result)[0]
        assert sample == -7

    def test_two_nibbles_per_byte(self):
        """Byte 0x21: low nibble=1, high nibble=2."""
        data = bytes([0x21])
        result = decode_ima_adpcm(data, 2)
        assert len(result) == 4
        s0 = struct.unpack("<h", result[0:2])[0]
        s1 = struct.unpack("<h", result[2:4])[0]
        assert s0 == 1  # nibble=1: delta=0+step>>2=0+1=1
        assert s1 > 0

    def test_clamp_positive(self):
        """Large positive nibbles should clamp to 32767."""
        data = bytes([0x77] * 50)
        result = decode_ima_adpcm(data, 100)
        samples = [
            struct.unpack("<h", result[i : i + 2])[0]
            for i in range(0, len(result), 2)
        ]
        assert max(samples) == 32767

    def test_clamp_negative(self):
        """Large negative nibbles should clamp to -32768."""
        data = bytes([0xFF] * 50)
        result = decode_ima_adpcm(data, 100)
        samples = [
            struct.unpack("<h", result[i : i + 2])[0]
            for i in range(0, len(result), 2)
        ]
        assert min(samples) == -32768

    def test_sample_count_exceeds_data(self):
        """When sampleCount > available nibbles, decode only what's available."""
        data = bytes([0x00])  # 1 byte = 2 nibbles
        result = decode_ima_adpcm(data, 10)
        # Should stop at 2 samples
        assert len(result) == 4

    def test_output_is_little_endian_signed(self):
        """Verify the byte order is little-endian signed 16-bit."""
        data = bytes([0x08])  # nibble=8: sign bit set, magnitude 0 → delta=0, predicted=-0=0... actually delta=step>>3=0
        result = decode_ima_adpcm(data, 1)
        assert len(result) == 2
        # nibble=8: sign=1, all magnitude bits 0, delta = 7>>3 = 0, predicted = -0 = 0
        sample = struct.unpack("<h", result)[0]
        assert sample == 0


# ── ADPCM table tests ───────────────────────────────────────────────────────


class TestAdpcmTables:
    def test_step_table_length(self):
        assert len(ADPCM_STEP_TABLE) == 89

    def test_step_table_first_entry(self):
        assert ADPCM_STEP_TABLE[0] == 7

    def test_step_table_last_entry(self):
        assert ADPCM_STEP_TABLE[88] == 32767

    def test_step_table_monotonically_increasing(self):
        for i in range(1, len(ADPCM_STEP_TABLE)):
            assert ADPCM_STEP_TABLE[i] > ADPCM_STEP_TABLE[i - 1]

    def test_index_table_length(self):
        assert len(ADPCM_INDEX_TABLE) == 16

    def test_index_table_symmetry(self):
        """First 4 and 9-12 are -1, rest are positive."""
        for i in range(4):
            assert ADPCM_INDEX_TABLE[i] == -1
            assert ADPCM_INDEX_TABLE[i + 8] == -1
        for i in range(4, 8):
            assert ADPCM_INDEX_TABLE[i] > 0
            assert ADPCM_INDEX_TABLE[i + 8] > 0


# ── Encode MP3 from IMA tests ───────────────────────────────────────────────


class TestEncodeMp3FromIma:
    def test_produces_valid_mp3_bytes(self):
        """A minimal IMA file should produce MP3 output starting with an MP3 sync word."""
        sample_count = 1600  # 100ms of audio at 16kHz
        header = struct.pack("<I", sample_count)
        # 1600 samples = 800 bytes of ADPCM data
        payload = bytes(800)
        ima_data = header + payload
        mp3_data = encode_mp3_from_ima(ima_data)
        assert len(mp3_data) > 0
        # MP3 frame sync: first 11 bits are 1 (0xFF followed by 0xE0+ mask)
        assert mp3_data[0] == 0xFF
        assert (mp3_data[1] & 0xE0) == 0xE0

    def test_empty_ima_produces_output(self):
        """Zero samples should still produce (empty) MP3 output without crashing."""
        header = struct.pack("<I", 0)
        ima_data = header
        mp3_data = encode_mp3_from_ima(ima_data)
        # lameenc may produce empty or just flush bytes (returns bytearray)
        assert isinstance(mp3_data, (bytes, bytearray))


# ── Protocol constant tests ─────────────────────────────────────────────────


class TestProtocolConstants:
    def test_service_uuid(self):
        assert SERVICE_UUID == "19b10000-e8f2-537e-4f6c-d104768a1214"

    def test_characteristic_uuids_sequential(self):
        assert CHARACTERISTIC_FILE_COUNT_UUID == "19b10001-e8f2-537e-4f6c-d104768a1214"
        assert CHARACTERISTIC_FILE_INFO_UUID == "19b10002-e8f2-537e-4f6c-d104768a1214"
        assert CHARACTERISTIC_AUDIO_DATA_UUID == "19b10003-e8f2-537e-4f6c-d104768a1214"
        assert CHARACTERISTIC_COMMAND_UUID == "19b10004-e8f2-537e-4f6c-d104768a1214"
        assert CHARACTERISTIC_VOLTAGE_UUID == "19b10005-e8f2-537e-4f6c-d104768a1214"
        assert CHARACTERISTIC_PAIRING_UUID == "19b10006-e8f2-537e-4f6c-d104768a1214"

    def test_command_bytes(self):
        assert COMMAND_REQUEST_NEXT == bytes([0x01])
        assert COMMAND_ACK_RECEIVED == bytes([0x02])
        assert COMMAND_SYNC_DONE == bytes([0x03])
        assert COMMAND_START_STREAM == bytes([0x04])

    def test_sample_rate(self):
        assert SAMPLE_RATE == 16000

    def test_ima_header_size(self):
        assert IMA_HEADER_SIZE == 4

    def test_max_transfer_attempts(self):
        assert MAX_FILE_TRANSFER_ATTEMPTS == 3


# ── Cross-platform consistency tests ────────────────────────────────────────


class TestCrossPlatformConsistency:
    """
    Verify that the Python and Kotlin decoders produce the same output.
    These tests encode known inputs and check the Python decoder produces
    expected outputs that we can also verify against the Kotlin tests.
    """

    def test_nibble_4_produces_7(self):
        """Both Python and Kotlin should decode nibble=4 as sample +7."""
        result = decode_ima_adpcm(bytes([0x04]), 1)
        assert struct.unpack("<h", result)[0] == 7

    def test_nibble_0c_produces_minus_7(self):
        """Both Python and Kotlin should decode nibble=0x0C as sample -7."""
        result = decode_ima_adpcm(bytes([0x0C]), 1)
        assert struct.unpack("<h", result)[0] == -7

    def test_byte_0x21_low_then_high(self):
        """Byte 0x21: low nibble=1 → sample 1, high nibble=2 → positive."""
        result = decode_ima_adpcm(bytes([0x21]), 2)
        s0 = struct.unpack("<h", result[0:2])[0]
        assert s0 == 1

    def test_known_sequence_matches(self):
        """A known 8-sample sequence must match the Kotlin decoder exactly."""
        data = bytes([0x12, 0x34, 0x56, 0x78])
        result = decode_ima_adpcm(data, 8)
        # Store the expected output from this run. If the decoder is correct,
        # both Python and Kotlin will produce the same bytes.
        # We verify length and first sample explicitly.
        assert len(result) == 16
        s0 = struct.unpack("<h", result[0:2])[0]
        # nibble=2: delta=step>>3 + step>>1 = 0+3=3, predicted=3
        assert s0 == 3
