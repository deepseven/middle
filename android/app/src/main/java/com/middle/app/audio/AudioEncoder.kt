package com.middle.app.audio

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMuxer
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

const val SAMPLE_RATE = 16000
private const val AAC_BIT_RATE = 64000
private const val CODEC_TIMEOUT_MICROSECONDS = 10_000L

object AudioEncoder {

    /**
     * Encode signed 16-bit little-endian PCM (mono, 16kHz) into an M4A file
     * using Android's built-in MediaCodec AAC encoder.
     *
     * We use AAC/M4A instead of MP3 because Android has no built-in MP3
     * encoder, and the OpenAI transcription API accepts M4A just fine.
     */
    fun encodeToM4a(pcm16: ByteArray, outputFile: File) {
        val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, SAMPLE_RATE, 1)
        format.setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
        format.setInteger(MediaFormat.KEY_BIT_RATE, AAC_BIT_RATE)

        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        codec.start()

        val muxer = MediaMuxer(outputFile.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        var trackIndex = -1
        var muxerStarted = false

        val bufferInfo = MediaCodec.BufferInfo()
        var inputOffset = 0
        var inputDone = false

        try {
            while (true) {
                // Feed input.
                if (!inputDone) {
                    val inputIndex = codec.dequeueInputBuffer(CODEC_TIMEOUT_MICROSECONDS)
                    if (inputIndex >= 0) {
                        val inputBuffer = codec.getInputBuffer(inputIndex)!!
                        val remaining = pcm16.size - inputOffset
                        if (remaining <= 0) {
                            codec.queueInputBuffer(
                                inputIndex, 0, 0, 0,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM,
                            )
                            inputDone = true
                        } else {
                            val chunkSize = minOf(remaining, inputBuffer.capacity())
                            inputBuffer.put(pcm16, inputOffset, chunkSize)
                            val presentationTimeUs = (inputOffset.toLong() / 2) * 1_000_000 / SAMPLE_RATE
                            codec.queueInputBuffer(inputIndex, 0, chunkSize, presentationTimeUs, 0)
                            inputOffset += chunkSize
                        }
                    }
                }

                // Drain output.
                val outputIndex = codec.dequeueOutputBuffer(bufferInfo, CODEC_TIMEOUT_MICROSECONDS)
                when {
                    outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        trackIndex = muxer.addTrack(codec.outputFormat)
                        muxer.start()
                        muxerStarted = true
                    }
                    outputIndex >= 0 -> {
                        val outputBuffer = codec.getOutputBuffer(outputIndex)!!
                        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                            // Codec config data — muxer handles this via the format.
                            bufferInfo.size = 0
                        }
                        if (bufferInfo.size > 0 && muxerStarted) {
                            outputBuffer.position(bufferInfo.offset)
                            outputBuffer.limit(bufferInfo.offset + bufferInfo.size)
                            muxer.writeSampleData(trackIndex, outputBuffer, bufferInfo)
                        }
                        codec.releaseOutputBuffer(outputIndex, false)

                        if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                            break
                        }
                    }
                }
            }
        } finally {
            codec.stop()
            codec.release()
            if (muxerStarted) {
                muxer.stop()
                muxer.release()
            }
        }
    }

    /**
     * Decode an IMA ADPCM file and encode to M4A in one step.
     */
    fun encodeFromIma(imaFileData: ByteArray, outputFile: File) {
        val pcm16 = ImaAdpcmDecoder.decodeFile(imaFileData)
        Log.d(TAG, "[SyncDebug] decodeFile() produced ${pcm16.size} PCM bytes.")
        encodeToM4a(pcm16, outputFile)
        Log.d(TAG, "[SyncDebug] encodeToM4a() wrote ${outputFile.length()} bytes to ${outputFile.absolutePath}.")
    }

    /**
     * Decode an M4A file back to signed 16-bit LE PCM wrapped in a WAV
     * container. Used by the custom STT path which expects audio/wav.
     */
    fun m4aToWav(m4aFile: File): ByteArray {
        val extractor = MediaExtractor()
        extractor.setDataSource(m4aFile.absolutePath)

        // Find the audio track.
        var audioTrackIndex = -1
        var audioFormat: MediaFormat? = null
        for (i in 0 until extractor.trackCount) {
            val format = extractor.getTrackFormat(i)
            if (format.getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true) {
                audioTrackIndex = i
                audioFormat = format
                break
            }
        }
        require(audioTrackIndex >= 0 && audioFormat != null) { "No audio track in ${m4aFile.name}" }

        extractor.selectTrack(audioTrackIndex)

        val codec = MediaCodec.createDecoderByType(
            audioFormat.getString(MediaFormat.KEY_MIME)!!,
        )
        codec.configure(audioFormat, null, null, 0)
        codec.start()

        val pcmOut = ByteArrayOutputStream()
        val bufferInfo = MediaCodec.BufferInfo()
        var extractorDone = false

        try {
            while (true) {
                // Feed compressed data.
                if (!extractorDone) {
                    val inIdx = codec.dequeueInputBuffer(CODEC_TIMEOUT_MICROSECONDS)
                    if (inIdx >= 0) {
                        val inBuf = codec.getInputBuffer(inIdx)!!
                        val sampleSize = extractor.readSampleData(inBuf, 0)
                        if (sampleSize < 0) {
                            codec.queueInputBuffer(
                                inIdx, 0, 0, 0,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM,
                            )
                            extractorDone = true
                        } else {
                            codec.queueInputBuffer(
                                inIdx, 0, sampleSize, extractor.sampleTime, 0,
                            )
                            extractor.advance()
                        }
                    }
                }

                // Drain decoded PCM.
                val outIdx = codec.dequeueOutputBuffer(bufferInfo, CODEC_TIMEOUT_MICROSECONDS)
                if (outIdx >= 0) {
                    if (bufferInfo.size > 0) {
                        val outBuf = codec.getOutputBuffer(outIdx)!!
                        outBuf.position(bufferInfo.offset)
                        outBuf.limit(bufferInfo.offset + bufferInfo.size)
                        val chunk = ByteArray(bufferInfo.size)
                        outBuf.get(chunk)
                        pcmOut.write(chunk)
                    }
                    codec.releaseOutputBuffer(outIdx, false)
                    if (bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                        break
                    }
                }
            }
        } finally {
            codec.stop()
            codec.release()
            extractor.release()
        }

        val pcmData = pcmOut.toByteArray()
        val sampleRate = audioFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE)
        val channels = audioFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
        return wrapPcmAsWav(pcmData, sampleRate, channels)
    }

    private fun wrapPcmAsWav(pcm: ByteArray, sampleRate: Int, channels: Int): ByteArray {
        val bitsPerSample = 16
        val byteRate = sampleRate * channels * bitsPerSample / 8
        val blockAlign = channels * bitsPerSample / 8
        val dataSize = pcm.size
        val headerSize = 44

        val wav = ByteBuffer.allocate(headerSize + dataSize).order(ByteOrder.LITTLE_ENDIAN)
        // RIFF header
        wav.put("RIFF".toByteArray())
        wav.putInt(headerSize - 8 + dataSize)
        wav.put("WAVE".toByteArray())
        // fmt sub-chunk
        wav.put("fmt ".toByteArray())
        wav.putInt(16)                     // sub-chunk size
        wav.putShort(1)                    // PCM format
        wav.putShort(channels.toShort())
        wav.putInt(sampleRate)
        wav.putInt(byteRate)
        wav.putShort(blockAlign.toShort())
        wav.putShort(bitsPerSample.toShort())
        // data sub-chunk
        wav.put("data".toByteArray())
        wav.putInt(dataSize)
        wav.put(pcm)

        return wav.array()
    }

    private const val TAG = "AudioEncoder"
}
