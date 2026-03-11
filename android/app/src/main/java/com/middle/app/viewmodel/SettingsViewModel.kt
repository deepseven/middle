package com.middle.app.viewmodel

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.middle.app.data.Settings
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

class SettingsViewModel(application: Application) : AndroidViewModel(application) {

    private val settings = Settings(application)

    private val _openAiApiKey = MutableStateFlow(settings.openAiApiKey)
    val openAiApiKey: StateFlow<String> = _openAiApiKey

    private val _elevenLabsApiKey = MutableStateFlow(settings.elevenLabsApiKey)
    val elevenLabsApiKey: StateFlow<String> = _elevenLabsApiKey

    private val _transcriptionProvider = MutableStateFlow(settings.transcriptionProvider)
    val transcriptionProvider: StateFlow<String> = _transcriptionProvider

    private val _customSttUrl = MutableStateFlow(settings.customSttUrl)
    val customSttUrl: StateFlow<String> = _customSttUrl

    private val _customSttApiKey = MutableStateFlow(settings.customSttApiKey)
    val customSttApiKey: StateFlow<String> = _customSttApiKey

    private val _backgroundSyncEnabled = MutableStateFlow(settings.backgroundSyncEnabled)
    val backgroundSyncEnabled: StateFlow<Boolean> = _backgroundSyncEnabled

    private val _transcriptionEnabled = MutableStateFlow(settings.transcriptionEnabled)
    val transcriptionEnabled: StateFlow<Boolean> = _transcriptionEnabled

    private val _webhookEnabled = MutableStateFlow(settings.webhookEnabled)
    val webhookEnabled: StateFlow<Boolean> = _webhookEnabled

    private val _webhookUrl = MutableStateFlow(settings.webhookUrl)
    val webhookUrl: StateFlow<String> = _webhookUrl

    private val _webhookBodyTemplate = MutableStateFlow(settings.webhookBodyTemplate)
    val webhookBodyTemplate: StateFlow<String> = _webhookBodyTemplate

    private val _isPaired = MutableStateFlow(settings.isPaired)
    val isPaired: StateFlow<Boolean> = _isPaired

    private val _pairingToken = MutableStateFlow(settings.pairingToken)
    val pairingToken: StateFlow<String> = _pairingToken

    fun setOpenAiApiKey(key: String) {
        settings.openAiApiKey = key
        _openAiApiKey.value = key
    }

    fun setElevenLabsApiKey(key: String) {
        settings.elevenLabsApiKey = key
        _elevenLabsApiKey.value = key
    }

    fun setTranscriptionProvider(provider: String) {
        settings.transcriptionProvider = provider
        _transcriptionProvider.value = provider
    }

    fun setCustomSttUrl(url: String) {
        settings.customSttUrl = url
        _customSttUrl.value = url
    }

    fun setCustomSttApiKey(key: String) {
        settings.customSttApiKey = key
        _customSttApiKey.value = key
    }

    fun setBackgroundSync(enabled: Boolean) {
        settings.backgroundSyncEnabled = enabled
        _backgroundSyncEnabled.value = enabled
    }

    fun setTranscription(enabled: Boolean) {
        settings.transcriptionEnabled = enabled
        _transcriptionEnabled.value = enabled
    }

    fun setWebhookEnabled(enabled: Boolean) {
        settings.webhookEnabled = enabled
        _webhookEnabled.value = enabled
    }

    fun setWebhookUrl(url: String) {
        settings.webhookUrl = url
        _webhookUrl.value = url
    }

    fun setWebhookBodyTemplate(template: String) {
        settings.webhookBodyTemplate = template
        _webhookBodyTemplate.value = template
    }

    fun unpairPendant() {
        settings.clearPairing()
        _isPaired.value = false
        _pairingToken.value = ""
    }
}
