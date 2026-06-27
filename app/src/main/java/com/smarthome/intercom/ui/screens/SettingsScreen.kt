package com.smarthome.intercom.ui.screens

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.smarthome.intercom.R
import com.smarthome.intercom.call.CallUiState
import com.smarthome.intercom.config.DeviceIdentity
import com.smarthome.intercom.ui.CallViewModel
import com.smarthome.intercom.ui.theme.AnswerGreen
import com.smarthome.intercom.ui.theme.DeclineRed
import kotlinx.coroutines.launch

/** Settings (frontend spec §3.4): identity, permissions with quick fixes, about. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    viewModel: CallViewModel,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    val identity by viewModel.deviceConfig.identity.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()

    var alias by remember(identity.alias) { mutableStateOf(identity.alias) }
    var dstAddr by remember(identity.dstAddr) { mutableStateOf(identity.dstAddr) }
    var doorName by remember(identity.doorName) { mutableStateOf(identity.doorName) }

    // Bumped on resume so permission statuses refresh after the user returns
    // from a system settings screen or a permission dialog.
    var refreshTick by remember { mutableIntStateOf(0) }
    LifecycleResumeEffect(Unit) {
        refreshTick++
        onPauseOrDispose { }
    }

    val micGranted = remember(refreshTick) {
        ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
    }
    val notificationsEnabled = remember(refreshTick) {
        NotificationManagerCompat.from(context).areNotificationsEnabled()
    }
    val batteryExempt = remember(refreshTick) {
        val pm = context.getSystemService(PowerManager::class.java)
        pm.isIgnoringBatteryOptimizations(context.packageName)
    }

    val micLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { refreshTick++ }
    val notifLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { refreshTick++ }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.settings_title)) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.open_settings))
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
        ) {
            // --- Identity ---
            SectionTitle(stringResource(R.string.settings_identity))
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    OutlinedTextField(
                        value = alias,
                        onValueChange = { alias = it },
                        label = { Text(stringResource(R.string.settings_unit_name)) },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(12.dp))
                    OutlinedTextField(
                        value = dstAddr,
                        onValueChange = { dstAddr = it },
                        label = { Text(stringResource(R.string.settings_address)) },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(12.dp))
                    OutlinedTextField(
                        value = doorName,
                        onValueChange = { doorName = it },
                        label = { Text(stringResource(R.string.settings_door)) },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(12.dp))
                    Text(
                        text = "${stringResource(R.string.settings_serial)}: ${identity.serial}",
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(16.dp))
                    Button(
                        onClick = {
                            scope.launch {
                                viewModel.deviceConfig.save(
                                    DeviceIdentity(alias, identity.serial, dstAddr, doorName),
                                )
                            }
                        },
                        modifier = Modifier.align(Alignment.End),
                    ) { Text(stringResource(R.string.save)) }
                }
            }

            Spacer(Modifier.height(24.dp))

            // --- Permissions ---
            SectionTitle(stringResource(R.string.settings_permissions))
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(4.dp)) {
                    PermissionRow(
                        label = stringResource(R.string.perm_mic),
                        granted = micGranted,
                    ) { micLauncher.launch(Manifest.permission.RECORD_AUDIO) }

                    PermissionRow(
                        label = stringResource(R.string.perm_notifications),
                        granted = notificationsEnabled,
                    ) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            notifLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                        } else {
                            context.startActivity(
                                Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                                    .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName),
                            )
                        }
                    }

                    PermissionRow(
                        label = stringResource(R.string.perm_battery),
                        granted = batteryExempt,
                    ) {
                        @Suppress("BatteryLife")
                        context.startActivity(
                            Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                                .setData(Uri.parse("package:${context.packageName}")),
                        )
                    }
                }
            }

            Spacer(Modifier.height(24.dp))

            // --- About ---
            SectionTitle(stringResource(R.string.settings_about))
            val versionName = remember {
                runCatching {
                    context.packageManager.getPackageInfo(context.packageName, 0).versionName
                }.getOrNull() ?: "1.0"
            }
            Text(
                text = stringResource(R.string.settings_version, versionName),
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(4.dp),
            )
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.titleLarge,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(vertical = 8.dp),
    )
}

@Composable
private fun PermissionRow(label: String, granted: Boolean, onFix: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurface)
            Text(
                text = stringResource(if (granted) R.string.perm_granted else R.string.perm_missing),
                style = MaterialTheme.typography.labelLarge,
                color = if (granted) AnswerGreen else DeclineRed,
            )
        }
        if (!granted) {
            OutlinedButton(onClick = onFix) { Text(stringResource(R.string.perm_fix)) }
        }
    }
}

// Keeps the unused-parameter warning away when previewing with a static state.
@Suppress("unused")
private val previewState = CallUiState()
