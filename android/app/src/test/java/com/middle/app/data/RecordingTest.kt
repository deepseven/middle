package com.middle.app.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.time.LocalDateTime

class RecordingTest {

    @Test
    fun `fromFile parses valid filename`() {
        val file = File("/tmp/recordings/recording_20260315_143022_0.m4a")
        // fromFile will fail because the file doesn't exist on disk (duration=0),
        // but it should still parse the filename pattern. Since audioFile.length()
        // returns 0 for nonexistent files, durationSeconds will be 0.
        val recording = Recording.fromFile(file)
        assertNotNull("Should parse valid filename", recording)
        assertEquals(
            LocalDateTime.of(2026, 3, 15, 14, 30, 22),
            recording!!.timestamp,
        )
    }

    @Test
    fun `fromFile returns null for invalid filename`() {
        assertNull(Recording.fromFile(File("/tmp/foo.m4a")))
        assertNull(Recording.fromFile(File("/tmp/recording_bad_0.m4a")))
        assertNull(Recording.fromFile(File("/tmp/recording_20260315_143022.m4a")))
    }

    @Test
    fun `fromFile returns null for wrong extension`() {
        assertNull(Recording.fromFile(File("/tmp/recording_20260315_143022_0.mp3")))
    }

    @Test
    fun `fromFile parses index suffix correctly`() {
        // Index can be multi-digit
        val recording = Recording.fromFile(
            File("/tmp/recordings/recording_20260101_000000_42.m4a"),
        )
        assertNotNull(recording)
        assertEquals(
            LocalDateTime.of(2026, 1, 1, 0, 0, 0),
            recording!!.timestamp,
        )
    }

    @Test
    fun `hasTranscript returns false when no transcript file`() {
        // Transcript file path is derived but doesn't exist on disk
        val recording = Recording(
            audioFile = File("/nonexistent/audio.m4a"),
            transcriptFile = null,
            timestamp = LocalDateTime.now(),
            durationSeconds = 1.0f,
        )
        assertFalse(recording.hasTranscript)
    }

    @Test
    fun `transcriptText returns null when no transcript`() {
        val recording = Recording(
            audioFile = File("/nonexistent/audio.m4a"),
            transcriptFile = null,
            timestamp = LocalDateTime.now(),
            durationSeconds = 1.0f,
        )
        assertNull(recording.transcriptText)
    }

    @Test
    fun `durationSeconds calculated from file size`() {
        // Duration = fileSize / 8000. A nonexistent file has size 0.
        val recording = Recording.fromFile(
            File("/tmp/recording_20260315_143022_0.m4a"),
        )
        assertNotNull(recording)
        assertEquals(0.0f, recording!!.durationSeconds, 0.001f)
    }

    @Test
    fun `fromFile timestamp format handles midnight`() {
        val recording = Recording.fromFile(
            File("/tmp/recording_20260101_000000_0.m4a"),
        )
        assertNotNull(recording)
        assertEquals(LocalDateTime.of(2026, 1, 1, 0, 0, 0), recording!!.timestamp)
    }

    @Test
    fun `fromFile timestamp format handles end of day`() {
        val recording = Recording.fromFile(
            File("/tmp/recording_20261231_235959_0.m4a"),
        )
        assertNotNull(recording)
        assertEquals(LocalDateTime.of(2026, 12, 31, 23, 59, 59), recording!!.timestamp)
    }
}
