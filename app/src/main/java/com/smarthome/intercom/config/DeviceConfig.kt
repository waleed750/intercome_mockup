package com.smarthome.intercom.config

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import java.util.UUID

/** This install's editable identity (INT-A4). */
data class DeviceIdentity(
    val alias: String,
    val serial: String,
    val dstAddr: String,
    val doorName: String,
)

private val Context.dataStore by preferencesDataStore(name = "intercom_config")

/**
 * Persists the per-install identity in DataStore and exposes it as a hot
 * [StateFlow] so the discovery responder can read the current value
 * synchronously. Defaults are generated once on first run and persisted, so
 * `serial`/`dstAddr` stay stable across restarts and two installs don't collide.
 */
class DeviceConfig(private val context: Context, scope: CoroutineScope) {

    private object Keys {
        val ALIAS = stringPreferencesKey("alias")
        val SERIAL = stringPreferencesKey("serial")
        val DST_ADDR = stringPreferencesKey("dst_addr")
        val DOOR = stringPreferencesKey("door_name")
    }

    private val defaults = generateDefaults()

    val identity = context.dataStore.data
        .map { prefs ->
            DeviceIdentity(
                alias = prefs[Keys.ALIAS] ?: defaults.alias,
                serial = prefs[Keys.SERIAL] ?: defaults.serial,
                dstAddr = prefs[Keys.DST_ADDR]?.takeIf { it.isValidAddress() } ?: defaults.dstAddr,
                doorName = prefs[Keys.DOOR] ?: defaults.doorName,
            )
        }
        .stateIn(scope, SharingStarted.Eagerly, defaults)

    init {
        // Persist generated defaults once so they don't change on restart.
        scope.launch {
            context.dataStore.edit { prefs ->
                if (prefs[Keys.ALIAS] == null) prefs[Keys.ALIAS] = defaults.alias
                if (prefs[Keys.SERIAL] == null) prefs[Keys.SERIAL] = defaults.serial
                if (!prefs[Keys.DST_ADDR].orEmpty().isValidAddress()) {
                    prefs[Keys.DST_ADDR] = defaults.dstAddr
                }
                if (prefs[Keys.DOOR] == null) prefs[Keys.DOOR] = defaults.doorName
            }
        }
    }

    suspend fun save(identity: DeviceIdentity) {
        context.dataStore.edit { prefs ->
            prefs[Keys.ALIAS] = identity.alias.ifBlank { defaults.alias }
            prefs[Keys.SERIAL] = identity.serial.ifBlank { defaults.serial }
            prefs[Keys.DST_ADDR] = identity.dstAddr.takeIf { it.isValidAddress() } ?: defaults.dstAddr
            prefs[Keys.DOOR] = identity.doorName.ifBlank { defaults.doorName }
        }
    }

    private fun generateDefaults(): DeviceIdentity {
        val suffix = (1000..9999).random()
        return DeviceIdentity(
            alias = "Indoor $suffix",
            serial = UUID.randomUUID().toString().replace("-", "").take(12),
            dstAddr = (1..999).random().toString(),
            doorName = "Front Door",
        )
    }

    private fun String.isValidAddress(): Boolean {
        val address = trim().toIntOrNull()
        return address != null && address in 1..999
    }
}
