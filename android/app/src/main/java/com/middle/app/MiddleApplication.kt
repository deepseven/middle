package com.middle.app

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import com.middle.app.data.RecordingsRepository
import com.middle.app.data.WebhookRetryQueue
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob

class MiddleApplication : Application() {
    lateinit var repository: RecordingsRepository
    lateinit var retryQueue: WebhookRetryQueue

    private val applicationScope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun onCreate() {
        super.onCreate()
        repository = RecordingsRepository(this)
        retryQueue = WebhookRetryQueue(this, applicationScope)
        createNotificationChannel()
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            SYNC_CHANNEL_ID,
            getString(R.string.sync_notification_channel),
            NotificationManager.IMPORTANCE_LOW,
        )
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(channel)
    }

    companion object {
        const val SYNC_CHANNEL_ID = "middle_sync"
        const val SYNC_NOTIFICATION_ID = 1
    }
}
