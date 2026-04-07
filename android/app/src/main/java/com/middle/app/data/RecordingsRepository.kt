package com.middle.app.data

import android.content.Context
import android.util.Log
import com.middle.app.audio.AudioEncoder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.withContext
import java.io.File

class RecordingsRepository(context: Context) {

    private val recordingsDirectory = File(context.filesDir, "recordings").also { it.mkdirs() }

    private val _recordings = MutableStateFlow<List<Recording>>(emptyList())
    val recordings: StateFlow<List<Recording>> = _recordings

    val directory: File get() = recordingsDirectory

    init {
        refresh()
    }

    fun refresh() {
        val files = recordingsDirectory.listFiles { file -> file.extension == "m4a" }
            ?: emptyArray()
        _recordings.value = files
            .mapNotNull { Recording.fromFile(it) }
            .sortedByDescending { it.timestamp }
        Log.d(TAG, "[SyncDebug] refresh() found ${_recordings.value.size} recording(s).")
    }

    /**
     * Decode IMA ADPCM data, encode to M4A, and save to disk.
     */
    suspend fun saveEncodedRecording(imaData: ByteArray, filename: String): File {
        val file = File(recordingsDirectory, filename)
        withContext(Dispatchers.IO) {
            AudioEncoder.encodeFromIma(imaData, file)
        }
        Log.d(TAG, "[SyncDebug] encodeFromIma() output path=${file.absolutePath} size=${file.length()} bytes.")
        refresh()
        return file
    }

    /**
     * Encode raw signed 16-bit LE PCM (mono, 16 kHz) to M4A and save to disk.
     * Used by the PTT recording path which captures directly from the microphone.
     */
    suspend fun savePcmRecording(pcm16: ByteArray, filename: String): File {
        val file = File(recordingsDirectory, filename)
        withContext(Dispatchers.IO) {
            AudioEncoder.encodeToM4a(pcm16, file)
        }
        Log.d(TAG, "[SyncDebug] savePcmRecording() output path=${file.absolutePath} size=${file.length()} bytes.")
        refresh()
        return file
    }

    companion object {
        private const val TAG = "RecordingsRepo"
    }

    suspend fun deleteRecording(recording: Recording) {
        withContext(Dispatchers.IO) {
            recording.audioFile.delete()
            val transcriptFile = File(
                recording.audioFile.parent,
                recording.audioFile.nameWithoutExtension + ".txt",
            )
            if (transcriptFile.exists()) {
                transcriptFile.delete()
            }
        }
        refresh()
    }

    suspend fun deleteAllRecordings() {
        withContext(Dispatchers.IO) {
            recordingsDirectory.listFiles()?.forEach { it.delete() }
        }
        refresh()
    }

    suspend fun saveTranscript(text: String, audioFile: File): File {
        val transcriptFile = File(audioFile.parent, audioFile.nameWithoutExtension + ".txt")
        withContext(Dispatchers.IO) {
            transcriptFile.writeText(text)
        }
        refresh()
        return transcriptFile
    }
}
