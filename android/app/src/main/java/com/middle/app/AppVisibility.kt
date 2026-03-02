package com.middle.app

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

object AppVisibility {
    private val _isForeground = MutableStateFlow(false)
    val isForeground: StateFlow<Boolean> = _isForeground

    fun setForeground(isForeground: Boolean) {
        _isForeground.value = isForeground
    }
}
