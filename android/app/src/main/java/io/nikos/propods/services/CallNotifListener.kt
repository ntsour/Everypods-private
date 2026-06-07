/*
    ProPods - AirPods liberated from Apple's ecosystem
    Copyright (C) 2025 ProPods contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

package io.nikos.propods.services

import android.app.Notification
import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * Watches ongoing-call notifications from VoIP apps (Teams, Viber, WhatsApp, ...) and
 * caches their action PendingIntents. AirPodsService can then call [setMuted] /
 * [hangUp] / [answerCall] / [declineCall] to fire the right one — the app
 * reacts as if the user tapped the button in the notification, keeping its
 * in-app UI in sync.
 *
 * Requires the user to grant Notification access (Settings → Apps → Special
 * access → Notification access). Use [isAccessGranted] / [openAccessSettings]
 * from UI to drive the grant flow.
 */
class CallNotifListener : NotificationListenerService() {

    companion object {
        private const val TAG = "CallNotifListener"

        private val WATCHED_PACKAGES = setOf(
            "com.microsoft.teams",
            "com.microsoft.teams.ipphone",
            "com.microsoft.teams2",
            "com.viber.voip",
            "com.whatsapp",
        )

        @Volatile private var muteAction: Notification.Action? = null
        @Volatile private var unmuteAction: Notification.Action? = null
        @Volatile private var hangUpAction: Notification.Action? = null
        @Volatile private var answerAction: Notification.Action? = null
        @Volatile private var declineAction: Notification.Action? = null
        @Volatile private var lastSeenKey: String? = null

        /** Canonical mute state derived from the app's notification buttons.
         *  true = app shows "Unmute" (i.e. currently muted)
         *  false = app shows "Mute" (i.e. currently unmuted)
         *  null = no active call notification seen yet */
        @Volatile var teamsMuted: Boolean? = null
            private set

        /** Called on the main thread whenever the app's in-notification mute state
         *  flips (not on first detection — only on subsequent changes). */
        @Volatile var onMuteStateChanged: ((muted: Boolean) -> Unit)? = null

        /** Returns the app's canonical mute state, or null if no call is active. */
        fun isTeamsMuted(): Boolean? = teamsMuted

        fun isAccessGranted(context: Context): Boolean {
            val flat = Settings.Secure.getString(
                context.contentResolver, "enabled_notification_listeners"
            ) ?: return false
            val cn = "${context.packageName}/${CallNotifListener::class.java.name}"
            return flat.split(":").any { it.trim() == cn }
        }

        fun openAccessSettings(context: Context) {
            val intent = android.content.Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(intent)
        }

        fun setMuted(muted: Boolean): Boolean {
            val action = if (muted) muteAction else unmuteAction
            if (action == null) {
                Log.d(TAG, "setMuted($muted): no cached action")
                return false
            }
            return try {
                action.actionIntent.send()
                Log.d(TAG, "setMuted($muted): fired ${action.title}")
                true
            } catch (t: Throwable) {
                Log.w(TAG, "setMuted($muted) failed: ${t.message}")
                false
            }
        }

        fun hangUp(): Boolean {
            val action = hangUpAction ?: run {
                Log.d(TAG, "hangUp(): no cached action")
                return false
            }
            return try {
                action.actionIntent.send()
                Log.d(TAG, "hangUp(): fired ${action.title}")
                true
            } catch (t: Throwable) {
                Log.w(TAG, "hangUp() failed: ${t.message}")
                false
            }
        }

        fun answerCall(flags: Int = 0): Boolean {
            val action = answerAction ?: run {
                Log.d(TAG, "answerCall(): no cached action")
                return false
            }
            return try {
                if (flags != 0) {
                    val ctx = ServiceManager.getService()?.applicationContext
                    val fillIn = Intent().apply { addFlags(flags) }
                    action.actionIntent.send(ctx, 0, fillIn)
                    Log.d(TAG, "answerCall(): fired ${action.title} (flags=$flags, ctx=${ctx?.javaClass?.simpleName})")
                } else {
                    action.actionIntent.send()
                    Log.d(TAG, "answerCall(): fired ${action.title}")
                }
                true
            } catch (t: Throwable) {
                Log.w(TAG, "answerCall() failed: ${t.message}")
                false
            }
        }

        fun declineCall(): Boolean {
            val action = declineAction ?: run {
                Log.d(TAG, "declineCall(): no cached action")
                return false
            }
            return try {
                action.actionIntent.send()
                Log.d(TAG, "declineCall(): fired ${action.title}")
                true
            } catch (t: Throwable) {
                Log.w(TAG, "declineCall() failed: ${t.message}")
                false
            }
        }
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        Log.d(TAG, "Listener connected")
        // Re-scan currently posted notifications so we pick up an in-progress call.
        try {
            activeNotifications?.forEach { handle(it) }
        } catch (t: Throwable) {
            Log.w(TAG, "scan active notifications failed: ${t.message}")
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        handle(sbn)
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.packageName !in WATCHED_PACKAGES) return
        if (sbn.key == lastSeenKey) {
            Log.d(TAG, "Call notification removed; clearing cached actions")
            muteAction = null
            unmuteAction = null
            hangUpAction = null
            answerAction = null
            declineAction = null
            lastSeenKey = null
            teamsMuted = null
        }
    }

    private fun handle(sbn: StatusBarNotification) {
        if (sbn.packageName !in WATCHED_PACKAGES) return
        val n = sbn.notification ?: return
        val actions = n.actions ?: return

        // Verbose: log every watched notification's action titles for debugging.
        val titles = actions.map { it.title?.toString().orEmpty() }
        Log.d(TAG, "Scanning ${sbn.packageName} notification (key=${sbn.key}, actions=${titles})")

        var foundMute: Notification.Action? = null
        var foundUnmute: Notification.Action? = null
        var foundHangUp: Notification.Action? = null
        var foundAnswer: Notification.Action? = null
        var foundDecline: Notification.Action? = null
        for (a in actions) {
            val title = a.title?.toString().orEmpty()
            val lower = title.lowercase()
            // Order matters: "unmute" contains "mute".
            if (lower.contains("unmute") || lower.contains("réactiver") || lower.contains("activar")) {
                foundUnmute = a
            } else if (lower.contains("mute") || lower.contains("muet") || lower.contains("silenc") || lower.contains("stumm")) {
                foundMute = a
            } else if (lower.contains("hang") || lower.contains("end call") || lower.contains("colgar") ||
                lower.contains("raccrocher") || lower.contains("auflegen") || lower.contains("finalizar")) {
                foundHangUp = a
            } else if (lower.contains("answer") || lower.contains("accept") ||
                lower.contains("contestar") || lower.contains("répondre") ||
                lower.contains("annehmen") || lower.contains("atender")) {
                foundAnswer = a
            } else if (lower.contains("decline") || lower.contains("reject") ||
                lower.contains("rechazar") || lower.contains("refuser") ||
                lower.contains("ablehnen") || lower.contains("rejeitar")) {
                foundDecline = a
            }
        }

        if (foundMute != null || foundUnmute != null || foundHangUp != null ||
            foundAnswer != null || foundDecline != null) {
            // Guard against false positives from non-call notifications that
            // coincidentally have similarly-named buttons (e.g. WhatsApp message
            // notifications also have a "Mute" button for conversation muting).
            // Require at least 2 call-related actions in the same notification
            // before caching — real call notifications always have either
            // mute+hangup (active call) or answer+decline (ringing).
            val callActionCount = listOf(
                foundMute, foundUnmute, foundHangUp, foundAnswer, foundDecline
            ).count { it != null }
            if (callActionCount < 2) {
                Log.d(TAG, "Ignoring ${sbn.packageName} notification with only $callActionCount call-related action(s) — likely not a call")
                return
            }
            muteAction = foundMute ?: muteAction
            unmuteAction = foundUnmute ?: unmuteAction
            hangUpAction = foundHangUp ?: hangUpAction
            answerAction = foundAnswer ?: answerAction
            declineAction = foundDecline ?: declineAction
            lastSeenKey = sbn.key
            Log.d(
                TAG,
                "Cached actions from ${sbn.packageName}: mute=${foundMute?.title}, unmute=${foundUnmute?.title}, hangUp=${foundHangUp?.title}, answer=${foundAnswer?.title}, decline=${foundDecline?.title}"
            )

            // Incoming ringing VoIP call detected. Self-managed VoIP apps
            // (WhatsApp) don't trigger TelephonyManager CALL_STATE_RINGING,
            // so the AirPods never enter HFP call state and setupStemActions()
            // is never called — meaning single press events aren't forwarded
            // to the app. Kick stem action setup here to re-apply the config.
            if (foundAnswer != null && foundDecline != null) {
                Log.d(TAG, "Ringing VoIP call detected for ${sbn.packageName} — calling setupStemActions()")
                ServiceManager.getService()?.setupStemActions()
            }

            // Derive canonical mute state: "Unmute" button visible → currently muted,
            // "Mute" button visible → currently unmuted. Ambiguous if both or neither present.
            val newState: Boolean? = when {
                foundUnmute != null && foundMute == null -> true
                foundMute != null && foundUnmute == null -> false
                else -> null
            }
            if (newState != null && newState != teamsMuted) {
                val wasKnown = teamsMuted != null
                teamsMuted = newState
                if (wasKnown) {
                    Log.d(TAG, "Teams mute state changed → $newState (in-app button)")
                    onMuteStateChanged?.invoke(newState)
                } else {
                    Log.d(TAG, "Teams initial mute state detected: $newState")
                }
            }
        }
    }
}
