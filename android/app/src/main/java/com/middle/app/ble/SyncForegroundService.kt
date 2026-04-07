package com.middle.app.ble

import android.app.Service
import android.bluetooth.BluetoothManager
import android.content.ClipData
import android.content.ClipboardManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Intent
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.IBinder
import android.os.ParcelUuid
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import androidx.core.app.NotificationCompat
import com.middle.app.AppVisibility
import com.middle.app.MiddleApplication
import com.middle.app.R
import com.middle.app.TaskerReceiver
import com.middle.app.audio.AudioEncoder
import com.middle.app.audio.SAMPLE_RATE
import com.middle.app.data.RecordingsRepository
import com.middle.app.data.Settings
import com.middle.app.data.WebhookClient
import com.middle.app.data.WebhookLog
import com.middle.app.transcription.TranscriptionClient
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.SecureRandom
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class SyncForegroundService : Service() {

    private data class ScanProfile(
        val scanMode: Int,
        val windowMillis: Long,
        val periodMillis: Long,
    )

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var syncJob: Job? = null
    private var scanning = false

    private lateinit var repository: RecordingsRepository
    private lateinit var settings: Settings

    override fun onCreate() {
        super.onCreate()
        repository = (application as MiddleApplication).repository
        settings = Settings(this)
        _batteryVoltage.value = settings.lastBatteryVoltage
        startForegroundNotification(getString(R.string.sync_notification_idle))
        startScanLoop()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            TaskerReceiver.ACTION_PTT_START -> startPttRecording()
            TaskerReceiver.ACTION_PTT_STOP -> scope.launch { stopPttRecording() }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        stopScan()
        scope.cancel()
        super.onDestroy()
    }

    private fun startForegroundNotification(text: String) {
        val notification = NotificationCompat.Builder(this, MiddleApplication.SYNC_CHANNEL_ID)
            .setContentTitle(getString(R.string.sync_notification_title))
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setOngoing(true)
            .build()
        startForeground(MiddleApplication.SYNC_NOTIFICATION_ID, notification)
    }

    private fun updateNotification(text: String) {
        _syncState.value = text
        startForegroundNotification(text)
    }

    private fun startScanLoop() {
        scope.launch {
            updateNotification(getString(R.string.sync_notification_scanning))
            while (true) {
                // Skip scan if a sync job is currently active.
                if (syncJob?.isActive != true) {
                    val profile = currentScanProfile()
                    startScan(profile.scanMode)
                    delay(profile.windowMillis)
                    stopScan()
                    delayUntilNextScan(profile)
                } else {
                    // Sync is active, wait before checking again.
                    delay(500)
                }
            }
        }
    }

    private suspend fun delayUntilNextScan(profile: ScanProfile) {
        var remaining = profile.periodMillis - profile.windowMillis
        while (remaining > 0) {
            if (currentScanProfile() != profile) {
                return
            }
            val step = minOf(remaining, 250L)
            delay(step)
            remaining -= step
        }
    }

    private fun currentScanProfile(): ScanProfile {
        if (AppVisibility.isForeground.value) {
            return ScanProfile(
                scanMode = ScanSettings.SCAN_MODE_LOW_LATENCY,
                windowMillis = FOREGROUND_SCAN_WINDOW_MILLIS,
                periodMillis = FOREGROUND_SCAN_PERIOD_MILLIS,
            )
        }

        return ScanProfile(
            scanMode = ScanSettings.SCAN_MODE_LOW_LATENCY,
            windowMillis = BACKGROUND_SCAN_WINDOW_MILLIS,
            periodMillis = BACKGROUND_SCAN_PERIOD_MILLIS,
        )
    }

    private fun startScan(scanMode: Int) {
        val bluetoothManager = getSystemService(BluetoothManager::class.java)
        val adapter = bluetoothManager?.adapter ?: return
        val scanner = adapter.bluetoothLeScanner ?: return

        // When paired, filter by MAC so we only wake up for our pendant.
        // When not paired, filter by service UUID to discover any pendant.
        val filter = if (settings.isPaired) {
            ScanFilter.Builder()
                .setDeviceAddress(settings.pairedDeviceAddress)
                .build()
        } else {
            ScanFilter.Builder()
                .setServiceUuid(ParcelUuid(SERVICE_UUID))
                .build()
        }

        val scanSettings = ScanSettings.Builder()
            .setScanMode(scanMode)
            .build()

        try {
            scanner.startScan(listOf(filter), scanSettings, scanCallback)
            scanning = true
            Log.d(TAG, "BLE scan started.")
        } catch (exception: SecurityException) {
            Log.e(TAG, "BLE scan permission denied: $exception")
        }
    }

    private fun stopScan() {
        if (!scanning) return
        val bluetoothManager = getSystemService(BluetoothManager::class.java)
        val scanner = bluetoothManager?.adapter?.bluetoothLeScanner ?: return
        try {
            scanner.stopScan(scanCallback)
        } catch (exception: SecurityException) {
            Log.e(TAG, "Failed to stop scan: $exception")
        }
        scanning = false
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            // Avoid starting multiple sync jobs simultaneously.
            if (syncJob?.isActive == true) return

            Log.d(TAG, "Found pendant: ${result.device.address}")
            stopScan()
            syncJob = scope.launch { syncWithDevice(result) }
        }

        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "BLE scan failed with error code: $errorCode")
            // The scan loop will naturally retry on the next iteration.
        }
    }

    private suspend fun syncWithDevice(scanResult: ScanResult) {
        (application as MiddleApplication).retryQueue.startRetryLoopIfNeeded()
        val manager = PendantBleManager(this)
        try {
            updateNotification(getString(R.string.sync_notification_connecting))
            manager.connectTo(scanResult.device)
            Log.d(TAG, "Connected to pendant.")

            val pairingStatus = manager.readPairingStatus()
            if (settings.isPaired) {
                // We have a stored token — write it so the firmware can verify us.
                manager.writePairingToken(hexToBytes(settings.pairingToken))
                Log.d(TAG, "[sync] token sent to pendant.")
            } else {
                if (pairingStatus == 0x00) {
                    // Pendant is unclaimed — claim it with a fresh random token.
                    val token = generatePairingToken()
                    manager.writePairingToken(token)
                    settings.pairingToken = bytesToHex(token)
                    settings.pairedDeviceAddress = scanResult.device.address
                    Log.d(TAG, "[sync] pendant claimed, stored token and MAC.")
                } else {
                    // Pendant is already claimed by a different device.
                    Log.w(TAG, "[sync] pendant already claimed, skipping.")
                    return
                }
            }

            val millivolts = manager.readVoltageMillivolts()
            if (millivolts != null) {
                val volts = millivolts / 1000.0
                val formatted = "%.2fV".format(volts)
                _batteryVoltage.value = formatted
                settings.lastBatteryVoltage = formatted
                Log.d(TAG, "Battery voltage: $formatted ($millivolts mV)")
            } else {
                _batteryVoltage.value = "N/A"
                settings.lastBatteryVoltage = "N/A"
                Log.d(TAG, "Voltage characteristic not available.")
            }

            updateNotification(getString(R.string.sync_notification_syncing))
            val fileCount = manager.readFileCount()
            Log.d(TAG, "Pendant reports $fileCount pending recording(s).")
            WebhookLog.info("BLE: connected, $fileCount file(s) pending")

            if (fileCount == 0) {
                manager.syncDone()
                WebhookLog.info("BLE: no files, sync done")
                return
            }

            var skipTranscription = false

            // Enable notifications once for the whole session to avoid rapid
            // CCCD churn that destabilises the GATT link between files.
            manager.enableAudioNotifications()
            try {
                for (i in 0 until fileCount) {
                    Log.d(TAG, "Requesting file ${i + 1}/$fileCount...")
                    updateNotification("Syncing file ${i + 1}/$fileCount...")

                    val imaData = manager.requestNextFile()

                    // Empty files are corrupt or aborted recordings. ACK to delete
                    // them from the pendant and continue to the next file.
                    if (imaData == null) {
                        Log.d(TAG, "Skipping empty file ${i + 1}/$fileCount.")
                        manager.acknowledgeFile()
                        delay(300)
                        continue
                    }

                    Log.d(TAG, "Received ${imaData.size} bytes.")

                    val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
                    val filename = "recording_${timestamp}_$i.m4a"
                    val audioFile = repository.saveEncodedRecording(imaData, filename)
                    Log.d(TAG, "Saved $filename.")

                    manager.acknowledgeFile()
                    // Brief pause between files to let the pendant settle before
                    // the next COMMAND_REQUEST_NEXT, reducing GATT instability.
                    delay(300)

                    if (!skipTranscription && settings.transcriptionEnabled) {
                        val provider = settings.transcriptionProvider
                        val apiKey = getSelectedProviderApiKey()
                        if (provider == Settings.TRANSCRIPTION_PROVIDER_CUSTOM && settings.customSttUrl.isBlank()) {
                            val message = "Transcription skipped: missing custom endpoint URL"
                            Log.w(TAG, message)
                            WebhookLog.error("$message ($filename)")
                            updateNotification(message)
                            skipTranscription = true
                        } else if (provider != Settings.TRANSCRIPTION_PROVIDER_CUSTOM && apiKey.isEmpty()) {
                            val message = "Transcription skipped: missing ${providerDisplayName(provider)} API key"
                            Log.w(TAG, message)
                            WebhookLog.error("$message ($filename)")
                            updateNotification(message)
                            skipTranscription = true
                        } else {
                            scope.launch(Dispatchers.IO) {
                                try {
                                val client = TranscriptionClient(provider, apiKey, settings.customSttUrl.trim())
                                val text = client.transcribe(audioFile)
                                if (text != null) {
                                    repository.saveTranscript(text, audioFile)
                                    Log.d(TAG, "Saved transcript for $filename.")
                                    copyToClipboard(text)

                                    val webhookUrl = settings.webhookUrl.trim()
                                    if (settings.webhookEnabled && webhookUrl.isNotEmpty()) {
                                        val template = settings.webhookBodyTemplate.ifBlank {
                                            Settings.DEFAULT_WEBHOOK_BODY_TEMPLATE
                                        }
                                        WebhookLog.info("POST $webhookUrl ($filename)")
                                        val appRetryQueue = (application as MiddleApplication).retryQueue
                                        try {
                                            val result = WebhookClient.post(webhookUrl, text, template)
                                            if (result.success) {
                                                Log.d(TAG, "Webhook POST succeeded for $filename.")
                                                WebhookLog.info("${result.code} OK ($filename)")
                                            } else {
                                                Log.w(TAG, "Webhook POST failed with status ${result.code} for $filename.")
                                                WebhookLog.error("${result.code} ${result.message} ($filename): ${result.body}")
                                                if (result.code !in 400..499) {
                                                    appRetryQueue.enqueue(text, webhookUrl, template, filename)
                                                }
                                            }
                                        } catch (exception: Exception) {
                                            Log.w(TAG, "Webhook POST error for $filename: $exception")
                                            WebhookLog.error("$filename: ${exception::class.simpleName}: ${exception.message}")
                                            appRetryQueue.enqueue(text, webhookUrl, template, filename)
                                        }
                                    }
                                } else {
                                    // Disable further transcription attempts this
                                    // session if the first one fails, same as sync.py.
                                    val message = "Transcription failed (${providerDisplayName(provider)})"
                                    Log.w(TAG, message)
                                    WebhookLog.error("$message ($filename)")
                                    updateNotification(message)
                                    skipTranscription = true
                                }
                                } catch (exception: Exception) {
                                    Log.e(TAG, "Transcription coroutine failed for $filename: $exception")
                                    WebhookLog.error("Transcription error ($filename): ${exception::class.simpleName}: ${exception.message}")
                                }
                            }
                        }
                    }
                }

                manager.syncDone()
                Log.d(TAG, "Sync complete, $fileCount file(s) transferred.")
                WebhookLog.info("BLE: sync complete, $fileCount file(s)")
            } finally {
                try {
                    manager.disableAudioNotifications()
                } catch (exception: Exception) {
                    Log.w(TAG, "disableAudioNotifications error (connection may be dead): $exception")
                }
            }
        } catch (exception: Exception) {
            Log.e(TAG, "Sync failed: $exception")
            WebhookLog.error("BLE: sync failed: ${exception.message}")
        } finally {
            try {
                manager.disconnect().enqueue()
            } catch (exception: Exception) {
                Log.w(TAG, "Disconnect error: $exception")
            }
            updateNotification(getString(R.string.sync_notification_scanning))
        }
    }

    // --- PTT recording state ---

    private var audioRecord: AudioRecord? = null
    private var pttCaptureJob: Job? = null
    private val pcmBuffer = ByteArrayOutputStream()

    private fun findUsbMicrophone(): AudioDeviceInfo? {
        val audioManager = getSystemService(AUDIO_SERVICE) as AudioManager
        return audioManager
            .getDevices(AudioManager.GET_DEVICES_INPUTS)
            .firstOrNull { device ->
                device.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                device.type == AudioDeviceInfo.TYPE_USB_HEADSET
            }
    }

    @Suppress("DEPRECATION")
    private fun vibrate(pattern: LongArray) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val manager = getSystemService(VIBRATOR_MANAGER_SERVICE) as VibratorManager
                manager.defaultVibrator.vibrate(
                    VibrationEffect.createWaveform(pattern, -1),
                )
            } else {
                val vibrator = getSystemService(VIBRATOR_SERVICE) as Vibrator
                vibrator.vibrate(VibrationEffect.createWaveform(pattern, -1))
            }
        } catch (exception: Exception) {
            Log.w(TAG, "Vibration failed: $exception")
        }
    }

    private fun startPttRecording() {
        if (audioRecord != null) {
            Log.w(TAG, "PTT recording already in progress, ignoring start.")
            return
        }

        val bufferSize = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        if (bufferSize == AudioRecord.ERROR || bufferSize == AudioRecord.ERROR_BAD_VALUE) {
            Log.e(TAG, "Invalid AudioRecord buffer size: $bufferSize")
            WebhookLog.error("PTT: AudioRecord buffer size error")
            return
        }

        val recorder = try {
            AudioRecord(
                MediaRecorder.AudioSource.MIC,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                bufferSize,
            )
        } catch (exception: SecurityException) {
            Log.e(TAG, "RECORD_AUDIO permission denied: $exception")
            WebhookLog.error("PTT: microphone permission denied")
            return
        }

        if (recorder.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord failed to initialize.")
            WebhookLog.error("PTT: AudioRecord init failed")
            recorder.release()
            return
        }

        // Prefer USB microphone if attached, otherwise use internal mic.
        val usbMic = findUsbMicrophone()
        if (usbMic != null) {
            val accepted = recorder.setPreferredDevice(usbMic)
            Log.d(TAG, if (accepted) "PTT: USB mic selected: ${usbMic.productName}"
                       else "PTT: USB mic found but rejected, using internal mic")
        } else {
            Log.d(TAG, "PTT: no USB mic found, using internal mic")
        }

        pcmBuffer.reset()
        recorder.startRecording()
        audioRecord = recorder
        updateNotification("PTT recording…")
        Log.d(TAG, "PTT recording started.")
        WebhookLog.info("PTT: recording started")
        vibrate(longArrayOf(0, 100))

        pttCaptureJob = scope.launch(Dispatchers.IO) {
            val chunk = ByteArray(bufferSize)
            while (audioRecord?.recordingState == AudioRecord.RECORDSTATE_RECORDING) {
                val read = recorder.read(chunk, 0, chunk.size)
                if (read > 0) {
                    synchronized(pcmBuffer) {
                        pcmBuffer.write(chunk, 0, read)
                    }
                }
            }
        }
    }

    private suspend fun stopPttRecording() {
        val recorder = audioRecord ?: run {
            Log.w(TAG, "PTT stop called but no recording in progress.")
            return
        }

        recorder.stop()
        recorder.release()
        audioRecord = null
        pttCaptureJob?.join()
        pttCaptureJob = null

        val pcm: ByteArray
        synchronized(pcmBuffer) {
            pcm = pcmBuffer.toByteArray()
            pcmBuffer.reset()
        }

        if (pcm.isEmpty()) {
            Log.w(TAG, "PTT recording produced no audio data.")
            WebhookLog.error("PTT: empty recording, skipping")
            updateNotification(getString(R.string.sync_notification_scanning))
            return
        }

        val durationMs = (pcm.size.toLong() / 2) * 1000 / SAMPLE_RATE
        Log.d(TAG, "PTT recording stopped: ${pcm.size} PCM bytes (~${durationMs}ms).")
        WebhookLog.info("PTT: recorded ${durationMs}ms")
        vibrate(longArrayOf(0, 80, 80, 80))
        updateNotification("Encoding PTT recording…")

        val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val filename = "recording_${timestamp}_0.m4a"
        val audioFile = repository.savePcmRecording(pcm, filename)
        Log.d(TAG, "PTT saved: $filename (${audioFile.length()} bytes)")

        updateNotification(getString(R.string.sync_notification_scanning))

        // Run the same transcription + webhook pipeline as the BLE path.
        if (settings.transcriptionEnabled) {
            val provider = settings.transcriptionProvider
            val apiKey = getSelectedProviderApiKey()
            if (provider == Settings.TRANSCRIPTION_PROVIDER_CUSTOM && settings.customSttUrl.isBlank()) {
                WebhookLog.error("PTT transcription skipped: missing custom endpoint URL ($filename)")
            } else if (provider != Settings.TRANSCRIPTION_PROVIDER_CUSTOM && apiKey.isEmpty()) {
                WebhookLog.error("PTT transcription skipped: missing ${providerDisplayName(provider)} API key ($filename)")
            } else {
                scope.launch(Dispatchers.IO) {
                    try {
                        val client = TranscriptionClient(provider, apiKey, settings.customSttUrl.trim())
                        val text = client.transcribe(audioFile)
                        if (text != null) {
                            repository.saveTranscript(text, audioFile)
                            Log.d(TAG, "PTT transcript saved for $filename.")
                            copyToClipboard(text)

                            val webhookUrl = settings.webhookUrl.trim()
                            if (settings.webhookEnabled && webhookUrl.isNotEmpty()) {
                                val template = settings.webhookBodyTemplate.ifBlank {
                                    Settings.DEFAULT_WEBHOOK_BODY_TEMPLATE
                                }
                                WebhookLog.info("POST $webhookUrl ($filename)")
                                val appRetryQueue = (application as MiddleApplication).retryQueue
                                try {
                                    val result = WebhookClient.post(webhookUrl, text, template)
                                    if (result.success) {
                                        Log.d(TAG, "Webhook POST succeeded for $filename.")
                                        WebhookLog.info("${result.code} OK ($filename)")
                                    } else {
                                        Log.w(TAG, "Webhook POST failed with status ${result.code} for $filename.")
                                        WebhookLog.error("${result.code} ${result.message} ($filename): ${result.body}")
                                        if (result.code !in 400..499) {
                                            appRetryQueue.enqueue(text, webhookUrl, template, filename)
                                        }
                                    }
                                } catch (exception: Exception) {
                                    Log.w(TAG, "Webhook POST error for $filename: $exception")
                                    WebhookLog.error("$filename: ${exception::class.simpleName}: ${exception.message}")
                                    appRetryQueue.enqueue(text, webhookUrl, template, filename)
                                }
                            }
                        } else {
                            WebhookLog.error("PTT transcription failed (${providerDisplayName(provider)}) ($filename)")
                        }
                    } catch (exception: Exception) {
                        Log.e(TAG, "PTT transcription coroutine failed for $filename: $exception")
                        WebhookLog.error("PTT transcription error ($filename): ${exception::class.simpleName}: ${exception.message}")
                    }
                }
            }
        }
    }

    private fun generatePairingToken(): ByteArray {
        val bytes = ByteArray(16)
        SecureRandom().nextBytes(bytes)
        return bytes
    }

    private fun bytesToHex(bytes: ByteArray): String =
        bytes.joinToString("") { "%02x".format(it) }

    private fun hexToBytes(hex: String): ByteArray =
        ByteArray(hex.length / 2) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

    private fun getSelectedProviderApiKey(): String {
        return when (settings.transcriptionProvider) {
            Settings.TRANSCRIPTION_PROVIDER_OPENAI -> settings.openAiApiKey.trim()
            Settings.TRANSCRIPTION_PROVIDER_ELEVENLABS -> settings.elevenLabsApiKey.trim()
            Settings.TRANSCRIPTION_PROVIDER_CUSTOM -> settings.customSttApiKey.trim()
            else -> ""
        }
    }

    private suspend fun copyToClipboard(text: String) {
        withContext(Dispatchers.Main) {
            try {
                val clipboard = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
                clipboard.setPrimaryClip(ClipData.newPlainText("Transcription", text))
                Log.d(TAG, "Transcript copied to clipboard.")
            } catch (exception: Exception) {
                Log.w(TAG, "Clipboard copy failed: $exception")
            }
        }
    }

    private fun providerDisplayName(provider: String): String {
        return when (provider) {
            Settings.TRANSCRIPTION_PROVIDER_OPENAI -> "OpenAI"
            Settings.TRANSCRIPTION_PROVIDER_ELEVENLABS -> "ElevenLabs"
            Settings.TRANSCRIPTION_PROVIDER_CUSTOM -> "Custom"
            else -> provider
        }
    }

    companion object {
        private const val TAG = "SyncService"

        private val _syncState = MutableStateFlow("Idle")
        val syncState: StateFlow<String> = _syncState

        private val _batteryVoltage = MutableStateFlow("N/A")
        val batteryVoltage: StateFlow<String> = _batteryVoltage
    }
}
