package com.middle.app.transcription

import android.util.Log
import com.middle.app.audio.AudioEncoder
import com.middle.app.data.Settings
import com.middle.app.data.WebhookLog
import okhttp3.Headers
import okhttp3.MediaType
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.asRequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import okio.BufferedSink
import org.json.JSONObject
import java.io.File
import java.util.concurrent.TimeUnit

class TranscriptionClient(
    private val provider: String,
    private val apiKey: String,
    private val customUrl: String = "",
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
            Settings.TRANSCRIPTION_PROVIDER_CUSTOM -> transcribeCustom(audioFile)
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
            .addFormDataPart("language", "en")
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
                val bodyText = response.body?.string() ?: ""
                Log.e(TAG, "Transcription failed: ${response.code} $bodyText")
                WebhookLog.error("Transcription failed (OpenAI): ${response.code} $bodyText")
                null
            } else {
                val body = response.body?.string() ?: return null
                parseTranscriptText(body, "OpenAI")
            }
        } catch (exception: Exception) {
            Log.e(TAG, "Transcription request failed: $exception")
            WebhookLog.error("Transcription request failed (OpenAI): ${exception::class.simpleName}: ${exception.message}")
            null
        }
    }

    private fun transcribeElevenLabs(audioFile: File): String? {
        val mimeType = "audio/mp4"
        val requestBody = MultipartBody.Builder()
            .setType(MultipartBody.FORM)
            .addFormDataPart("model_id", ELEVENLABS_TRANSCRIPTION_MODEL)
            .addFormDataPart("language_code", "eng")
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
                val bodyText = response.body?.string() ?: ""
                Log.e(TAG, "Transcription failed: ${response.code} $bodyText")
                WebhookLog.error("Transcription failed (ElevenLabs): ${response.code} $bodyText")
                null
            } else {
                val body = response.body?.string() ?: return null
                parseTranscriptText(body, "ElevenLabs")
            }
        } catch (exception: Exception) {
            Log.e(TAG, "Transcription request failed: $exception")
            WebhookLog.error("Transcription request failed (ElevenLabs): ${exception::class.simpleName}: ${exception.message}")
            null
        }
    }

    private fun transcribeCustom(audioFile: File): String? {
        if (customUrl.isBlank()) {
            Log.e(TAG, "Custom STT URL is empty")
            WebhookLog.error("Transcription failed (Custom): URL not configured")
            return null
        }

        // The custom server expects audio/wav (like OpenAI Whisper).
        // Convert the M4A recording to WAV in memory before sending.
        val wavBytes = try {
            AudioEncoder.m4aToWav(audioFile)
        } catch (exception: Exception) {
            Log.e(TAG, "WAV conversion failed: $exception")
            WebhookLog.error("Transcription failed (Custom): WAV conversion error: ${exception.message}")
            return null
        }

        val wavMime = "audio/wav".toMediaType()
        val language = "en"

        // Build multipart body using addPart() with explicit headers instead
        // of addFormDataPart() for text fields.  OkHttp's addFormDataPart()
        // adds a per-part Content-Length header that Python's `requests`
        // library omits.  Some Whisper server implementations misparse
        // form fields when per-part Content-Length is present.
        val textFields = listOf(
            "model" to CUSTOM_TRANSCRIPTION_MODEL,
            "response_format" to "json",
            "language" to language,
            "temperature" to "0",
        )
        val bodyBuilder = MultipartBody.Builder()
            .setType(MultipartBody.FORM)
        for ((name, value) in textFields) {
            bodyBuilder.addPart(
                Headers.headersOf("Content-Disposition", "form-data; name=\"$name\""),
                noLengthBody(value),
            )
        }
        bodyBuilder.addFormDataPart(
            "file",
            "audio.wav",
            wavBytes.toRequestBody(wavMime),
        )

        Log.d(TAG, "Custom STT request: url=$customUrl language=$language wavSize=${wavBytes.size} (from ${audioFile.name} ${audioFile.length()} bytes)")
        for ((name, value) in textFields) {
            Log.d(TAG, "  form field: $name=$value")
        }

        return try {
            val requestBuilder = Request.Builder()
                .url(customUrl)
                .post(bodyBuilder.build())

            if (apiKey.isNotEmpty()) {
                requestBuilder.header("Authorization", "Bearer $apiKey")
            }

            val response = httpClient.newCall(requestBuilder.build()).execute()
            if (!response.isSuccessful) {
                val bodyText = response.body?.string() ?: ""
                Log.e(TAG, "Transcription failed: ${response.code} $bodyText")
                WebhookLog.error("Transcription failed (Custom): ${response.code} $bodyText")
                null
            } else {
                val body = response.body?.string() ?: return null
                Log.d(TAG, "Custom STT response: $body")
                parseTranscriptText(body, "Custom")
            }
        } catch (exception: Exception) {
            Log.e(TAG, "Transcription request failed: $exception")
            WebhookLog.error("Transcription request failed (Custom): ${exception::class.simpleName}: ${exception.message}")
            null
        }
    }

    private fun parseTranscriptText(body: String, providerDisplayName: String): String? {
        return try {
            val json = JSONObject(body)
            when {
                json.has("text") -> json.optString("text")
                json.has("transcription") -> json.optString("transcription")
                json.has("transcript") -> json.optString("transcript")
                else -> {
                    val message = "Transcription response missing text field ($providerDisplayName): $body"
                    Log.e(TAG, message)
                    WebhookLog.error(message)
                    null
                }
            }?.takeIf { it.isNotBlank() }
        } catch (exception: Exception) {
            val message = "Transcription response parse failed ($providerDisplayName): ${exception::class.simpleName}: ${exception.message}"
            Log.e(TAG, "$message. Body: $body")
            WebhookLog.error(message)
            null
        }
    }

    /**
     * A [RequestBody] for a plain-text value that reports unknown content
     * length (-1).  This prevents OkHttp from adding a per-part
     * Content-Length header in the multipart body, matching the encoding
     * produced by Python's `requests` library.
     */
    private fun noLengthBody(value: String): RequestBody = object : RequestBody() {
        override fun contentType(): MediaType? = null
        override fun contentLength(): Long = -1L
        override fun writeTo(sink: BufferedSink) { sink.writeUtf8(value) }
    }

    companion object {
        private const val TAG = "Transcription"
        private const val OPENAI_TRANSCRIPTION_MODEL = "gpt-4o-transcribe"
        private const val OPENAI_TRANSCRIPTION_URL = "https://api.openai.com/v1/audio/transcriptions"
        private const val ELEVENLABS_TRANSCRIPTION_MODEL = "scribe_v2"
        private const val ELEVENLABS_TRANSCRIPTION_URL = "https://api.elevenlabs.io/v1/speech-to-text"
        private const val CUSTOM_TRANSCRIPTION_MODEL = "whisper-1"

        private fun mimeTypeForFile(file: File): String {
            return when (file.extension.lowercase()) {
                "m4a", "mp4" -> "audio/mp4"
                "wav" -> "audio/wav"
                "mp3" -> "audio/mpeg"
                "ogg" -> "audio/ogg"
                "flac" -> "audio/flac"
                "webm" -> "audio/webm"
                else -> "application/octet-stream"
            }
        }
    }
}
