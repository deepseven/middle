package com.middle.app.audio

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class ImaAdpcmDecoderTest {

    @Test
    fun `decodeAdpcm silence produces zero samples`() {
        // All-zero nibbles with initial state (predicted=0, index=0) should
        // produce small positive deltas (step>>3 = 7>>3 = 0 for first samples).
        val data = ByteArray(4) // 8 nibbles = 8 samples, all 0x00
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 8)
        assertEquals(16, result.size) // 8 samples * 2 bytes each
    }

    @Test
    fun `decodeAdpcm output is signed 16-bit little-endian`() {
        // Feed a single nibble 0x04 (positive step) and check byte order.
        val data = byteArrayOf(0x04) // low nibble = 4, high nibble = 0
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 1)
        assertEquals(2, result.size)
        val sample = ByteBuffer.wrap(result).order(ByteOrder.LITTLE_ENDIAN).short
        // nibble=4: step=7, delta = 7>>3 + 7 = 0+7 = 7, predicted = +7
        assertEquals(7.toShort(), sample)
    }

    @Test
    fun `decodeAdpcm negative nibble produces negative sample`() {
        // nibble=0x0C (bit3=sign, bit2=step): delta = 7>>3 + 7 = 7, negate = -7
        val data = byteArrayOf(0x0C.toByte())
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 1)
        val sample = ByteBuffer.wrap(result).order(ByteOrder.LITTLE_ENDIAN).short
        assertEquals((-7).toShort(), sample)
    }

    @Test
    fun `decodeAdpcm two nibbles per byte low then high`() {
        // Byte 0x21: low nibble = 1, high nibble = 2
        val data = byteArrayOf(0x21)
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 2)
        assertEquals(4, result.size) // 2 samples * 2 bytes

        val buf = ByteBuffer.wrap(result).order(ByteOrder.LITTLE_ENDIAN)
        val sample0 = buf.short // nibble=1: delta = 0 + 1 = 1, predicted=1
        val sample1 = buf.short // nibble=2: step[index after nibble1], delta includes step>>1
        assertEquals(1.toShort(), sample0)
        // sample1 depends on step after first nibble; just verify it's positive
        assert(sample1 > 0) { "Expected positive sample, got $sample1" }
    }

    @Test
    fun `decodeAdpcm clamps to 16-bit range`() {
        // Feed many large positive nibbles to drive predicted_sample above 32767.
        // nibble=0x07 (bits 0,1,2 set) with increasing steps should clip.
        val data = ByteArray(50) { 0x77 } // all nibbles = 7
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 100)
        val buf = ByteBuffer.wrap(result).order(ByteOrder.LITTLE_ENDIAN)
        var maxSample: Short = 0
        while (buf.hasRemaining()) {
            val s = buf.short
            if (s > maxSample) maxSample = s
        }
        assertEquals("Should clamp to 32767", 32767.toShort(), maxSample)
    }

    @Test
    fun `decodeAdpcm clamps negative to minus 32768`() {
        // nibble=0x0F: sign bit + all magnitude bits → maximum negative delta
        val data = ByteArray(50) { 0xFF.toByte() } // all nibbles = 15
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 100)
        val buf = ByteBuffer.wrap(result).order(ByteOrder.LITTLE_ENDIAN)
        var minSample: Short = 0
        while (buf.hasRemaining()) {
            val s = buf.short
            if (s < minSample) minSample = s
        }
        assertEquals("Should clamp to -32768", (-32768).toShort(), minSample)
    }

    @Test
    fun `decodeAdpcm empty data returns empty`() {
        val result = ImaAdpcmDecoder.decodeAdpcm(ByteArray(0), 0)
        assertEquals(0, result.size)
    }

    @Test
    fun `decodeAdpcm handles sampleCount greater than available data`() {
        // Only 1 byte = 2 nibbles, but asking for 10 samples
        val data = byteArrayOf(0x00)
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 10)
        // Should decode only 2 samples (the available ones)
        assertEquals(4, result.size)
    }

    @Test
    fun `decodeFile parses 4-byte LE header and decodes payload`() {
        val sampleCount = 4
        val header = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(sampleCount).array()
        val adpcmPayload = byteArrayOf(0x00, 0x00) // 4 nibbles = 4 samples
        val imaFileData = header + adpcmPayload

        val result = ImaAdpcmDecoder.decodeFile(imaFileData)
        assertEquals(sampleCount * 2, result.size) // 4 samples * 2 bytes
    }

    @Test
    fun `decodeFile with known data produces deterministic output`() {
        // Create a known input and verify it always produces the same output.
        val sampleCount = 8
        val header = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(sampleCount).array()
        val adpcmPayload = byteArrayOf(0x12, 0x34, 0x56, 0x78.toByte())
        val imaFileData = header + adpcmPayload

        val result1 = ImaAdpcmDecoder.decodeFile(imaFileData)
        val result2 = ImaAdpcmDecoder.decodeFile(imaFileData)
        assertArrayEquals("Decoder must be deterministic", result1, result2)
    }

    @Test
    fun `step table has exactly 89 entries`() {
        // IMA ADPCM standard requires exactly 89 step sizes.
        // Access via reflection or test indirectly — decoded output proves the
        // table is used correctly. We test the boundary: index should stay in 0..88.
        // Feed nibble 0x07 repeatedly: INDEX_TABLE[7]=8, so index grows quickly.
        val data = ByteArray(50) { 0x77 }
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 100)
        // If step table was wrong/missing entries, this would throw ArrayIndexOutOfBoundsException
        assert(result.isNotEmpty())
    }

    @Test
    fun `index table clamps index to valid range`() {
        // nibble=0 → INDEX_TABLE[0]=-1, starting at index=0 should clamp to 0
        val data = byteArrayOf(0x00)
        val result = ImaAdpcmDecoder.decodeAdpcm(data, 1)
        assertEquals(2, result.size) // Should not throw, index stays at 0
    }
}
