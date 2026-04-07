package com.middle.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import androidx.core.content.ContextCompat
import com.middle.app.ble.SyncForegroundService

class TaskerReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return

        when (action) {
            ACTION_PTT_START, ACTION_PTT_STOP -> {
                Log.d(TAG, "Received: $action")
                val serviceIntent = Intent(context, SyncForegroundService::class.java).apply {
                    this.action = action
                }
                ContextCompat.startForegroundService(context, serviceIntent)
            }
            else -> Log.w(TAG, "Unknown action: $action")
        }
    }

    companion object {
        const val ACTION_PTT_START = "com.middle.app.ACTION_PTT_START"
        const val ACTION_PTT_STOP = "com.middle.app.ACTION_PTT_STOP"
        private const val TAG = "TaskerReceiver"
    }
}
