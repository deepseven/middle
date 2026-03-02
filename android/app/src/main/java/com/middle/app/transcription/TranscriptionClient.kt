package com.middle.app.transcription

import android.util.Log
import com.middle.app.data.Settings
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.asRequestBody
import org.json.JSONObject
import java.io.File
import java.util.concurrent.TimeUnit

class TranscriptionClient(
    private val provider: String,
    private val apiKey: String,
) {

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(120, TimeUnit.SECONDS)
        .writeTimeout(30, TimeUnit.SECONDS)
        .build()

    fun transcribe(audioFile: File): String? {
        return when (provider) {
            Settings.TRANSCRIPTION_PROVIDER_OPENAI -> transcribeOpenAi(audioFile)
            Settings.TRANSCRIPTION_PROVIDER_ELEVENLABS -> transcribeElevenLabs(audioFile)
            else -> {
                Log.e(TAG, "Unsupported transcription provider: $provider")
                null
            }
        }
    }

    private fun transcribeOpenAi(audioFile: File): String? {
        val mimeType = "audio/mp4"
        val requestBody = MultipartBody.Builder()
            .setType(MultipartBody.FORM)
            .addFormDataPart("model", OPENAI_TRANSCRIPTION_MODEL)
            .addFormDataPart("response_format", "json")
            .addFormDataPart(
                "file",
                audioFile.name,
                audioFile.asRequestBody(mimeType.toMediaType()),
            )
            .build()

        val request = Request.Builder()
            .url(OPENAI_TRANSCRIPTION_URL)
            .header("Authorization", "Bearer $apiKey")
            .post(requestBody)
            .build()

        return try {
            val response = httpClient.newCall(request).execute()
            if (!response.isSuccessful) {
                Log.e(TAG, "Transcription failed: ${response.code} ${response.body?.string()}")
                null
            } else {
                val body = response.body?.string() ?: return null
                JSONObject(body).getString("text")
            }
        } catch (exception: Exception) {
            Log.e(TAG, "Transcription request failed: $exception")
            null
        }
    }

    private fun transcribeElevenLabs(audioFile: File): String? {
        val mimeType = "audio/mp4"
        val requestBody = MultipartBody.Builder()
            .setType(MultipartBody.FORM)
            .addFormDataPart("model_id", ELEVENLABS_TRANSCRIPTION_MODEL)
            .addFormDataPart(
                "file",
                audioFile.name,
                audioFile.asRequestBody(mimeType.toMediaType()),
            )
            .build()

        val request = Request.Builder()
            .url(ELEVENLABS_TRANSCRIPTION_URL)
            .header("xi-api-key", apiKey)
            .post(requestBody)
            .build()

        return try {
            val response = httpClient.newCall(request).execute()
            if (!response.isSuccessful) {
                Log.e(TAG, "Transcription failed: ${response.code} ${response.body?.string()}")
                null
            } else {
                val body = response.body?.string() ?: return null
                JSONObject(body).getString("transcription")
            }
        } catch (exception: Exception) {
            Log.e(TAG, "Transcription request failed: $exception")
            null
        }
    }

    companion object {
        private const val TAG = "Transcription"
        private const val OPENAI_TRANSCRIPTION_MODEL = "gpt-4o-transcribe"
        private const val OPENAI_TRANSCRIPTION_URL = "https://api.openai.com/v1/audio/transcriptions"
        private const val ELEVENLABS_TRANSCRIPTION_MODEL = "scribe_v2"
        private const val ELEVENLABS_TRANSCRIPTION_URL = "https://api.elevenlabs.io/v1/speech-to-text"
    }
}
