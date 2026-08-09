// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.activities

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Toast
import org.dolphinemu.dolphinemu.NativeLibrary
import org.dolphinemu.dolphinemu.R

class OpenXRControllerMapperActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())

    private val pollMapper = object : Runnable {
        override fun run() {
            when (NativeLibrary.GetOpenXRControllerMapperState()) {
                STATE_APPLY_PENDING -> {
                    if (NativeLibrary.ApplyOpenXRControllerMapper()) {
                        Toast.makeText(
                            this@OpenXRControllerMapperActivity,
                            R.string.openxr_controller_mapper_applied,
                            Toast.LENGTH_SHORT
                        ).show()
                    }
                    finish()
                    return
                }
                STATE_APPLIED, STATE_CANCELLED -> {
                    finish()
                    return
                }
                STATE_FAILED -> {
                    Toast.makeText(
                        this@OpenXRControllerMapperActivity,
                        NativeLibrary.GetOpenXRControllerMapperFailure(),
                        Toast.LENGTH_LONG
                    ).show()
                    finish()
                    return
                }
            }
            handler.postDelayed(this, POLL_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        setContentView(View(this).apply { setBackgroundColor(Color.BLACK) })

        val port = intent.getIntExtra(EXTRA_CONTROLLER_PORT, -1)
        val targetType = intent.getIntExtra(EXTRA_TARGET_TYPE, TARGET_WII_REMOTE)
        if (!NativeLibrary.StartOpenXRControllerMapper(this, port, targetType)) {
            Toast.makeText(this, NativeLibrary.GetOpenXRControllerMapperFailure(), Toast.LENGTH_LONG)
                .show()
            finish()
            return
        }
        handler.post(pollMapper)
    }

    override fun onDestroy() {
        handler.removeCallbacks(pollMapper)
        NativeLibrary.StopOpenXRControllerMapper()
        super.onDestroy()
    }

    companion object {
        const val EXTRA_CONTROLLER_PORT = "controller_port"
        const val EXTRA_TARGET_TYPE = "target_type"
        const val TARGET_WII_REMOTE = 0
        const val TARGET_GAMECUBE_CONTROLLER = 1
        const val TARGET_HOTKEYS = 2
        private const val POLL_INTERVAL_MS = 100L
        private const val STATE_APPLY_PENDING = 3
        private const val STATE_APPLIED = 4
        private const val STATE_CANCELLED = 5
        private const val STATE_FAILED = 6
    }
}
