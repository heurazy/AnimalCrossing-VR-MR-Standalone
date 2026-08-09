// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.ui.launcher

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.dolphinemu.dolphinemu.BuildConfig
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.databinding.ActivityAnimalCrossingVrLauncherBinding
import org.dolphinemu.dolphinemu.features.settings.model.QuestVrSettings
import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag
import org.dolphinemu.dolphinemu.features.settings.ui.SettingsActivity
import org.dolphinemu.dolphinemu.model.GameFile
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.ContentHandler
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.Log

/**
 * Minimal Animal Crossing VR frontend.
 *
 * Dolphin remains the emulator/runtime behind the scenes, but Quest users only need to choose a
 * compatible Animal Crossing disc image once, press Play, and optionally open the VR-only settings
 * page. The selected SAF URI is persisted across launches.
 */
class AnimalCrossingVrLauncherActivity : AppCompatActivity() {
    private lateinit var binding: ActivityAnimalCrossingVrLauncherBinding
    private var staleQuestStopRequested = false

    private val preferences by lazy {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
    }

    private val romPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) {
            handleSelectedRom(uri)
        } else {
            refreshRomUi()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityAnimalCrossingVrLauncherBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.textVersion.text = getString(R.string.acvr_launcher_version, BuildConfig.VERSION_NAME)
        binding.buttonSettings.setOnClickListener { openVrSettings() }
        binding.buttonLaunch.setOnClickListener { launchGame() }
        binding.buttonChooseRom.setOnClickListener { openRomPicker() }
        binding.buttonChangeRom.setOnClickListener { openRomPicker() }

        refreshRomUi()
    }

    override fun onResume() {
        super.onResume()

        if (DirectoryInitialization.shouldStart(this)) {
            DirectoryInitialization.start(this)
        }

        stopStaleQuestEmulation()
        refreshRomUi()
    }

    private fun stopStaleQuestEmulation() {
        if (!staleQuestStopRequested && QuestVrSettings.isQuestBuild() && NativeLibrary.IsRunning()) {
            staleQuestStopRequested = true
            Log.warning("[ACVRLauncher] Stopping stale Quest emulation before showing launcher.")
            NativeLibrary.StopEmulation()
        }
    }

    private fun openRomPicker() {
        romPicker.launch(arrayOf("application/octet-stream", "application/x-iso9660-image", "*/*"))
    }

    private fun handleSelectedRom(uri: Uri) {
        binding.buttonLaunch.isEnabled = false
        binding.textRomName.text = getString(R.string.acvr_launcher_rom_checking)
        binding.textRomStatus.text = getString(R.string.acvr_launcher_rom_checking_description)
        binding.textRomStatus.setTextColor(getColor(R.color.acvr_text_secondary))

        try {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            Log.warning("[ACVRLauncher] Could not persist ROM URI permission: ${e.message}")
        }

        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            validateAndStoreRom(uri)
        }
    }

    private fun validateAndStoreRom(uri: Uri) {
        val uriString = uri.toString()
        val displayName = ContentHandler.getDisplayName(uriString) ?: uri.lastPathSegment ?: "ROM"
        val extension = displayName.substringAfterLast('.', "").lowercase()

        if (extension.isNotEmpty() && extension !in SUPPORTED_DISC_EXTENSIONS) {
            showRomError(getString(R.string.acvr_launcher_wrong_extension, displayName))
            return
        }

        val game = try {
            GameFile.parse(uriString)
        } catch (e: Exception) {
            Log.error("[ACVRLauncher] Failed to parse selected ROM: ${e.message}")
            null
        }

        if (game == null) {
            showRomError(getString(R.string.acvr_launcher_invalid_rom))
            return
        }

        val gameId = game.getGameId()
        val revision = game.getRevision()
        if (gameId != REQUIRED_GAME_ID || revision != REQUIRED_REVISION) {
            showRomError(
                getString(
                    R.string.acvr_launcher_wrong_game,
                    gameId.ifBlank { getString(R.string.acvr_launcher_unknown_game_id) },
                    revision
                )
            )
            return
        }

        preferences.edit()
            .putString(KEY_ROM_URI, uriString)
            .putString(KEY_ROM_NAME, displayName)
            .apply()

        refreshRomUi()
    }

    private fun showRomError(message: String) {
        preferences.edit().remove(KEY_ROM_URI).remove(KEY_ROM_NAME).apply()
        refreshRomUi()
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.acvr_launcher_rom_error_title)
            .setMessage(message)
            .setPositiveButton(R.string.acvr_launcher_choose_another_rom) { _, _ -> openRomPicker() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun refreshRomUi() {
        val romUri = getStoredRomUri()
        val hasRom = romUri != null

        binding.buttonLaunch.isEnabled = hasRom
        binding.buttonLaunch.alpha = if (hasRom) 1.0f else 0.45f
        binding.buttonChooseRom.visibility = if (hasRom) View.GONE else View.VISIBLE
        binding.buttonChangeRom.visibility = if (hasRom) View.VISIBLE else View.GONE

        if (hasRom) {
            val name = preferences.getString(KEY_ROM_NAME, null)
                ?: ContentHandler.getDisplayName(romUri.toString())
                ?: getString(R.string.acvr_launcher_selected_rom)
            binding.textRomName.text = name
            binding.textRomStatus.text = getString(R.string.acvr_launcher_rom_ready)
            binding.textRomStatus.setTextColor(getColor(R.color.acvr_success))
        } else {
            binding.textRomName.text = getString(R.string.acvr_launcher_no_rom)
            binding.textRomStatus.text = getString(R.string.acvr_launcher_no_rom_description)
            binding.textRomStatus.setTextColor(getColor(R.color.acvr_text_secondary))
        }
    }

    private fun getStoredRomUri(): Uri? {
        val value = preferences.getString(KEY_ROM_URI, null) ?: return null
        if (!ContentHandler.exists(value)) {
            preferences.edit().remove(KEY_ROM_URI).remove(KEY_ROM_NAME).apply()
            return null
        }
        return Uri.parse(value)
    }

    private fun launchGame() {
        val romUri = getStoredRomUri()
        if (romUri == null) {
            return
        }

        binding.buttonLaunch.isEnabled = false
        binding.buttonLaunch.text = getString(R.string.acvr_launcher_starting)

        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            EmulationActivity.launch(this, romUri.toString(), false)
        }
    }

    private fun openVrSettings() {
        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            SettingsActivity.launch(this, MenuTag.OPENXR)
        }
    }

    companion object {
        private const val PREFS_NAME = "animal_crossing_vr_launcher"
        private const val KEY_ROM_URI = "rom_uri"
        private const val KEY_ROM_NAME = "rom_name"
        private const val REQUIRED_GAME_ID = "GAFE01"
        private const val REQUIRED_REVISION = 0

        private val SUPPORTED_DISC_EXTENSIONS = setOf("gcm", "iso", "ciso", "gcz", "wia", "rvz")
    }
}
