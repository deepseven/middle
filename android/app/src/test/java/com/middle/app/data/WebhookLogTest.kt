package com.middle.app.data

import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class WebhookLogTest {

    @Before
    fun setUp() {
        // Clear any entries from previous tests by adding then verifying.
        // WebhookLog is a singleton, so we need to work with its current state.
        // We can't clear it directly, but we can test incrementally.
    }

    @Test
    fun `info adds entry with isError false`() = runBlocking {
        val sizeBefore = WebhookLog.entries.value.size
        WebhookLog.info("test info message")
        val entries = WebhookLog.entries.value
        assertTrue(entries.size > sizeBefore)
        assertFalse(entries.first().isError)
        assertEquals("test info message", entries.first().message)
    }

    @Test
    fun `error adds entry with isError true`() = runBlocking {
        WebhookLog.error("test error message")
        val entries = WebhookLog.entries.value
        assertTrue(entries.first().isError)
        assertEquals("test error message", entries.first().message)
    }

    @Test
    fun `entries are prepended newest first`() = runBlocking {
        WebhookLog.info("first")
        WebhookLog.info("second")
        val entries = WebhookLog.entries.value
        assertEquals("second", entries[0].message)
        assertEquals("first", entries[1].message)
    }

    @Test
    fun `max 50 entries enforced`() = runBlocking {
        // Add 60 entries to exceed the cap
        repeat(60) { WebhookLog.info("entry $it") }
        val entries = WebhookLog.entries.value
        assertTrue("Should have at most 50 entries", entries.size <= 50)
    }

    @Test
    fun `entry has timestamp string`() = runBlocking {
        WebhookLog.info("timestamped")
        val entry = WebhookLog.entries.value.first()
        assertTrue(
            "Timestamp should match HH:mm:ss pattern",
            entry.timestamp.matches(Regex("""\d{2}:\d{2}:\d{2}""")),
        )
    }

    @Test
    fun `entries is a StateFlow not null`() {
        val flow = WebhookLog.entries
        assertTrue(flow.value is List)
    }
}
