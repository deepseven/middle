package com.middle.app.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.UUID

/**
 * Tests that BLE constants match the expected protocol values defined in the
 * firmware (src/main.cpp) and sync script (sync.py). Any change to these
 * constants must be coordinated across all three codebases.
 */
class BleConstantsTest {

    @Test
    fun `service UUID matches firmware`() {
        assertEquals(
            UUID.fromString("19b10000-e8f2-537e-4f6c-d104768a1214"),
            SERVICE_UUID,
        )
    }

    @Test
    fun `file count characteristic UUID ends with 0001`() {
        assertEquals(
            UUID.fromString("19b10001-e8f2-537e-4f6c-d104768a1214"),
            CHARACTERISTIC_FILE_COUNT_UUID,
        )
    }

    @Test
    fun `file info characteristic UUID ends with 0002`() {
        assertEquals(
            UUID.fromString("19b10002-e8f2-537e-4f6c-d104768a1214"),
            CHARACTERISTIC_FILE_INFO_UUID,
        )
    }

    @Test
    fun `audio data characteristic UUID ends with 0003`() {
        assertEquals(
            UUID.fromString("19b10003-e8f2-537e-4f6c-d104768a1214"),
            CHARACTERISTIC_AUDIO_DATA_UUID,
        )
    }

    @Test
    fun `command characteristic UUID ends with 0004`() {
        assertEquals(
            UUID.fromString("19b10004-e8f2-537e-4f6c-d104768a1214"),
            CHARACTERISTIC_COMMAND_UUID,
        )
    }

    @Test
    fun `voltage characteristic UUID ends with 0005`() {
        assertEquals(
            UUID.fromString("19b10005-e8f2-537e-4f6c-d104768a1214"),
            CHARACTERISTIC_VOLTAGE_UUID,
        )
    }

    @Test
    fun `pairing characteristic UUID ends with 0006`() {
        assertEquals(
            UUID.fromString("19b10006-e8f2-537e-4f6c-d104768a1214"),
            CHARACTERISTIC_PAIRING_UUID,
        )
    }

    @Test
    fun `command bytes match protocol spec`() {
        assertEquals(0x01.toByte(), COMMAND_REQUEST_NEXT)
        assertEquals(0x02.toByte(), COMMAND_ACK_RECEIVED)
        assertEquals(0x03.toByte(), COMMAND_SYNC_DONE)
        assertEquals(0x04.toByte(), COMMAND_START_STREAM)
    }

    @Test
    fun `MTU is 517`() {
        assertEquals(517, REQUESTED_MTU)
    }

    @Test
    fun `max transfer attempts is 3`() {
        assertEquals(3, MAX_FILE_TRANSFER_ATTEMPTS)
    }

    @Test
    fun `transfer timeout is positive and reasonable`() {
        assertTrue(
            "Transfer timeout should be at least 10 seconds",
            TRANSFER_TOTAL_TIMEOUT_MILLIS >= 10_000L,
        )
        assertTrue(
            "Transfer timeout should be at most 5 minutes",
            TRANSFER_TOTAL_TIMEOUT_MILLIS <= 300_000L,
        )
    }

    @Test
    fun `GATT operation timeout is positive and less than transfer timeout`() {
        assertTrue(
            "GATT op timeout should be at least 5 seconds",
            GATT_OPERATION_TIMEOUT_MILLIS >= 5_000L,
        )
        assertTrue(
            "GATT op timeout should be less than transfer timeout",
            GATT_OPERATION_TIMEOUT_MILLIS < TRANSFER_TOTAL_TIMEOUT_MILLIS,
        )
    }

    @Test
    fun `scan windows are positive`() {
        assertTrue(BACKGROUND_SCAN_WINDOW_MILLIS > 0)
        assertTrue(BACKGROUND_SCAN_PERIOD_MILLIS > 0)
        assertTrue(FOREGROUND_SCAN_WINDOW_MILLIS > 0)
        assertTrue(FOREGROUND_SCAN_PERIOD_MILLIS > 0)
    }

    @Test
    fun `scan window does not exceed period`() {
        assertTrue(
            "Background scan window should be <= period",
            BACKGROUND_SCAN_WINDOW_MILLIS <= BACKGROUND_SCAN_PERIOD_MILLIS,
        )
        assertTrue(
            "Foreground scan window should be <= period",
            FOREGROUND_SCAN_WINDOW_MILLIS <= FOREGROUND_SCAN_PERIOD_MILLIS,
        )
    }

    @Test
    fun `all UUIDs share the same base`() {
        val base = "e8f2-537e-4f6c-d104768a1214"
        assertTrue(SERVICE_UUID.toString().endsWith(base))
        assertTrue(CHARACTERISTIC_FILE_COUNT_UUID.toString().endsWith(base))
        assertTrue(CHARACTERISTIC_FILE_INFO_UUID.toString().endsWith(base))
        assertTrue(CHARACTERISTIC_AUDIO_DATA_UUID.toString().endsWith(base))
        assertTrue(CHARACTERISTIC_COMMAND_UUID.toString().endsWith(base))
        assertTrue(CHARACTERISTIC_VOLTAGE_UUID.toString().endsWith(base))
        assertTrue(CHARACTERISTIC_PAIRING_UUID.toString().endsWith(base))
    }
}
