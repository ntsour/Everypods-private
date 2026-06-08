# Call Handling via AirPods Stem Presses

---

## User Experience

| Press | Ringing Call | Active Call |
|-------|-------------|-------------|
| **Single press** | Answer | Mute / Unmute |
| **Double press** | Reject / Decline | End call |

The mute-vs-end behavior on active calls is configurable: the `CALL_MANAGEMENT_CONFIG`
AACP control command (identifier `0x24`) byte 1 swaps which press is mute and which
is end:

| Config byte 1 | Single press | Double press |
|---|---|---|
| `0x03` (default) | Mute / Unmute | End call |
| `0x02` (flipped) | End call | Mute / Unmute |

Set from `CallControlsScreen.kt` — on toggle, writes `byteArrayOf(0x00, 0x02)` or
`byteArrayOf(0x00, 0x03)` to `CALL_MANAGEMENT_CONFIG`.

Each successful press plays an audible confirmation chime (high pitch for unmute /
accept, low pitch for mute / decline). The chime is layered 4× by `GestureFeedback`
to stay audible over an in-progress call.

---

## Platform Support Matrix

| Platform | Answer | End | Mute | Detection |
|----------|:------:|:---:|:----:|-----------|
| **Cellular (PSTN)** | `TelecomManager.acceptRingingCall()` | HFP native or `TelecomManager.endCall()` | `AudioManager.setMicrophoneMute()` | `TelephonyManager.CALL_STATE_RINGING/OFFHOOK` |
| **Microsoft Teams (incoming 1-on-1 call)** | `dispatchHeadsethookToTelecom()` (Telecom self-managed) | `dispatchHeadsethookToTelecom()` | Notification `mute`/`unmute` actions (keyword-matched) | Telecom Call object + `CallNotifListener` |
| **Microsoft Teams meeting (joined from inside Teams)** | n/a (joined manually) | Notification `Hang up` PendingIntent (Broadcast) | Notification `Mute`/`Unmute` actions (keyword-matched) | Telecom self-managed + `CallNotifListener` (active state) |
| **WhatsApp** | `dispatchHeadsethookToTelecom()` (Telecom self-managed) | `dispatchHeadsethookToTelecom()` | `AudioManager.setMicrophoneMute()` (no Mute action in notif) | Telecom Call + `CallNotifListener` |
| **Viber** | `CallNotifListener.answerCall()` with `BAL_ALLOWED` ActivityOptions | Notification `Hang up` (Broadcast PI) | Notification `Mute` action | **NOT registered with Telecom** — relies entirely on `CallNotifListener` |
| **Other self-managed VoIP** (Signal, Telegram, etc.) | Telecom dispatch if registered, else notification `answerCall` | Same | `AudioManager.setMicrophoneMute()` + notification action when available | Same |

Three things separate the apps:

1. **Telecom registration.** WhatsApp, Teams 1-on-1, Teams meetings register a
   self-managed `ConnectionService` with Telecom (we observe `prop=[ self_mng]` on
   their Telecom Call objects). Viber does **not** — its calls exist purely as
   `CallStyle` notifications + audio focus.
2. **PendingIntent type of the Answer action.** Teams and WhatsApp use `startService`
   or `startActivity` PendingIntents that Android's Background Activity Launch (BAL)
   restriction would normally block from a foreground-service caller. Viber's Answer
   is an Activity PendingIntent. We always fire notification PendingIntents with
   `ActivityOptions.setPendingIntentBackgroundActivityStartMode(MODE_BACKGROUND_ACTIVITY_START_ALLOWED)`
   on API 34+ — harmless for Broadcast PIs, essential for Activity PIs.
3. **Whether the active-call notification uses `Notification.CallStyle`.** Teams
   meeting active notification uses `BigTextStyle` (no `CallStyle` extras). We
   fall through to keyword matching for those.

---

## Three-Tier Action Resolution

`CallNotifListener` resolves the five call-related actions (`answer`, `decline`,
`hangUp`, `mute`, `unmute`) from each watched notification using a three-tier
pipeline. Each tier is independent and refines what previous tiers left null.

### Tier 1 — `Notification.CallStyle` extras (locale-independent, structural)

Apps that build call notifications with `Notification.CallStyle.forIncomingCall()`
or `.forOngoingCall()` store the action PendingIntents in `notification.extras`
under stable framework keys:

- `Notification.EXTRA_ANSWER_INTENT`  → `"android.answerIntent"`
- `Notification.EXTRA_DECLINE_INTENT` → `"android.declineIntent"`
- `Notification.EXTRA_HANG_UP_INTENT` → `"android.hangUpIntent"`

`Notification.EXTRA_CALL_TYPE` returns one of `CALL_TYPE_INCOMING (1)`,
`CALL_TYPE_ONGOING (2)`, `CALL_TYPE_SCREENING (3)`, or `UNSPECIFIED (0)`.

For each PendingIntent retrieved from extras we either find the matching
`Notification.Action` in `notification.actions[]` (preferred — preserves any
extras / RemoteInputs the app attached) or synthesise a wrapper `Action`. Tier 1
hits are completely independent of language, device locale, or notification
button labels.

**Verified to use Tier 1 (incoming ringing notification):**
- WhatsApp (`channel=voip_notification_*`)
- Viber  (`channel=calls_1_c1`)
- Microsoft Teams 1-on-1 incoming call (`channel=com.microsoft.teams.Calls`)

**Does NOT use Tier 1 (no `CallStyle` extras):**
- Microsoft Teams meeting active state (`channel=com.microsoft.teams.CallsOngoing`,
  built with `BigTextStyle`) — falls through to Tier 3.

### Tier 2 — Position convention

Strictly gated: only runs when
- `notification.category == CATEGORY_CALL`,
- `EXTRA_CALL_TYPE == CALL_TYPE_INCOMING` (explicit),
- Tier 1 + Tier 3 didn't already resolve answer/decline,
- `actions.size >= 2`.

Then we cache `actions[0]` as `declineAction` and `actions[1]` as `answerAction`,
mirroring `Notification.CallStyle.forIncomingCall(person, declineIntent, answerIntent)`
arg order.

Tier 2 deliberately does **not** run for active calls or for notifications that
omit `EXTRA_CALL_TYPE` (Teams active does both) — position-based guessing on
unknown shapes would mis-classify `Unmute` as `decline` and `Hang up` as
`answer`, which used to cause the "single press ends Teams meeting instead of
muting" bug.

### Tier 3 — Keyword match (multi-locale title substring)

Title strings, lowercased and ASCII-folded where common, matched substring-style
against five keyword lists covering English, Spanish, French, German, Italian,
Portuguese, Dutch, Polish, Turkish, Russian, Greek, Chinese (simplified +
traditional), Japanese, Korean, Arabic, Hebrew.

This is the safety net for any app that doesn't expose `CallStyle` extras
(Teams active is the prime example) and where position convention is too risky.
It also resolves `mute` / `unmute` for all apps — `Notification.CallStyle` has
no mute extra so keywords are the only signal for mute state.

Order matters: `unmute` is checked before `mute` (substring trap on the word
"mute" inside "unmute").

### False-positive guard

After all three tiers, the cache only updates if at least 2 call-related actions
were resolved **or** Tier 1 hit (which is structural and trustworthy on its
own). This prevents a WhatsApp message-thread notification's "Mute" chat-mute
button being mistaken for a call-mute action.

### Caching with fallback

Cached actions are updated only if the new notification resolves that action;
missing ones retain their previous value:

```kotlin
muteAction    = foundMute    ?: muteAction
unmuteAction  = foundUnmute  ?: unmuteAction
hangUpAction  = foundHangUp  ?: hangUpAction
answerAction  = foundAnswer  ?: answerAction
declineAction = foundDecline ?: declineAction
```

This preserves cached actions across notification updates that briefly omit one
action (some apps post intermediate notifications with only the mute toggle
during state flaps).

---

## Background Activity Launch (BAL) bypass

Android 14+ blocks foreground-services from launching activities via PendingIntent
(`BAL_BLOCK`) unless the sender explicitly opts in. Viber's Answer PendingIntent
targets `com.viber.voip/.phone.PhoneFragmentActivity` (`startActivity`) — without
opt-in the launch is silently dropped and the call stays ringing.

All four dispatch helpers in `CallNotifListener` (`answerCall`, `declineCall`,
`hangUp`, `setMuted`) fire the cached PendingIntent with:

```kotlin
val opts = if (Build.VERSION.SDK_INT >= 34) {
    ActivityOptions.makeBasic()
        .setPendingIntentBackgroundActivityStartMode(
            ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED
        )
        .toBundle()
} else null
val fillIn = Intent().apply { addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) }
action.actionIntent.send(ctx, 0, fillIn, null, null, null, opts)
```

The `ActivityOptions` bundle is harmless for Broadcast / Service PendingIntents
(Teams Decline, WhatsApp Decline, etc.) — the framework simply ignores it. For
Activity PendingIntents (Viber Answer, Viber Hangup) it lifts the BAL block.

This was discovered via a temporary 10-method investigation harness on
`test/viber-answer-methods`. The full method matrix is preserved in that branch
for future reference.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        AirPods Stem Press                        │
└─────────────────────────────────────────────────────────────────┘
          │                                  │
          ▼ (HFP)                            ▼ (AACP — opcode 0x19)
 ┌──────────────────┐            ┌──────────────────────────┐
 │  Bluetooth Stack  │            │  AirPodsService           │
 │  (system)         │            │  onStemPressReceived()    │
 │                   │            │    ↓                      │
 │  → HFP AT command │            │  isInAnyCall()            │
 │  → TelecomManager │            │    ↓                      │
 └──────────────────┘            │  handleCallStemPress()     │
                                  │    ├─ ringing → answer()   │
                                  │    └─ active  → mute/end() │
                                  │         ↓                  │
                                  │  dispatchHeadsethookTo     │
                                  │  Telecom()  ───────────┐   │
                                  │  CallNotifListener      │   │
                                  │  .answerCall()          │   │
                                  │  .hangUp()              │   │
                                  │  .setMuted()            │   │
                                  └─────────────────────────┘   │
                                                              │   │
                                                              ▼   │
 ┌─────────────────────────────────────────────────────────────┐  │
 │                    TelecomManager (system)                    │  │
 │                         HeadsetMediaButton                    │◄─┘
 │                         MediaSession                         │
 │                                                              │
 │  SHORT_PRESS → CallsManager.onMediaButton()                  │
 │    ├─ ringing  → Call.answer()  (via ConnectionService)     │
 │    └─ active   → Call.disconnect()                           │
 └─────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
 ┌─────────────────────────────────────────────────────────────┐
 │              WhatsApp SelfManagedConnectionService            │
 │                                                              │
 │  Connection.onAnswer()   → answer call                       │
 │  Connection.onReject()   → reject call                       │
 └─────────────────────────────────────────────────────────────┘
```

---

## Call State Detection — `isInAnyCall()`

```
Returns true if ANY of these is true:
  1. isInCall              (TelephonyManager.CALL_STATE_OFFHOOK — cellular / Teams 1-on-1)
  2. isCallRinging         (TelephonyManager.CALL_STATE_RINGING — managed calls)
  3. isVoIPCallActive      (manually set; legacy code paths)
  4. audioManager.mode == MODE_IN_COMMUNICATION   (catches most VoIP audio)
  5. TelecomManager.isInCall    (catches self-managed VoIP — WhatsApp, Teams meeting)
  6. CallNotifListener.isVoipNotifRinging   (Viber and other Telecom-bypassing apps)
  7. CallNotifListener.isVoipNotifActive    (active-call notification cached, no Telecom)
```

The last two flags are set by `CallNotifListener` when it caches answer+decline
(ringing) or hangUp/mute/unmute (active) actions. They're the only signal that
catches Viber rings — Viber doesn't trigger TelephonyManager and doesn't
register with Telecom.

---

## Stem Press Routing — `handleCallStemPress()`

```
handleCallStemPress(pressType):
  isRinging = isCallRinging || voipRinging || notifRinging

  Is ringing?
    SINGLE_PRESS → answerCall()
    DOUBLE_PRESS → rejectCall()

  Active call (isInCall || isVoIPCallActive || notifActive)?
    Read CALL_MANAGEMENT_CONFIG byte[1]:
      0x03 (default) → single=mute, double=end
      0x02 (flipped) → single=end, double=mute
    Mute press → toggleMicMute()
    End press  → rejectCall()
```

---

## Answer Path

```kotlin
// AirPodsService.answerCall()
1. dispatchHeadsethookToTelecom()
   PRIMARY for cellular + managed/self-managed Telecom apps
   (WhatsApp, Teams 1-on-1, Teams meeting).
   Bypasses MediaSessionService's global-priority drop via
   MediaController.dispatchMediaButtonEvent() on Telecom's
   HeadsetMediaButton session.
   If true → return.

2. TelecomManager.acceptRingingCall()
   Managed-call fallback. Requires ANSWER_PHONE_CALLS permission.
   Silently fails for self-managed VoIP — logged as
   "self-mgd accept ignored from non-privileged app".

3. CallNotifListener.answerCall()
   Notification PendingIntent fallback. Fires the cached
   Answer action with BAL_ALLOWED ActivityOptions.
   This is the path that handles Viber (no Telecom registration).
```

---

## Reject / End Path

```kotlin
// AirPodsService.rejectCall()
Determine state:
  telecomInCall = TelecomManager.isInCall
  ringing = isCallRinging
  active = !ringing && (isInCall || isVoIPCallActive || telecomInCall)

For active calls:
  1. dispatchHeadsethookToTelecom()  — ends call via Telecom session.
     Works for any Telecom-registered call.

For ringing or fallback after Telecom dispatch fails:
  2. TelecomManager.endCall() (API 29+) / ITelephony.endCall() reflection
  3. CallNotifListener.hangUp()    — active-call notification action
  4. CallNotifListener.declineCall() — ringing notification action
```

---

## Mute Toggle — `toggleMicMute()`

```
1. Determine current mute state:
   - Canonical: CallNotifListener.isMuteShownByAppNotif()
     (reads the notification button title — "Unmute" visible ⇒ currently muted).
   - Fallback: AudioManager.isMicrophoneMute.
   This ensures correct toggle direction even if the user muted from inside the app.

2. Toggle: nowMuted = !wasMuted.

3. Record lastSelfMuteAtMs = elapsedRealtime
   (Arms the echo guard BEFORE setting state — see below.)

4. AudioManager.setMicrophoneMute(nowMuted)

5. CallNotifListener.setMuted(nowMuted)
   Fires the cached Mute/Unmute PendingIntent with BAL_ALLOWED. Keeps the VoIP
   app's in-app UI in sync.

6. gestureDetector.audio.playConfirmation(!nowMuted)
   Stacked 4× by GestureFeedback for ~+12 dB perceived loudness so the chime
   cuts through call audio.
```

### Mute echo guard

When `setMuted` fires a notification action, the watched app responds by posting
an updated notification. `CallNotifListener.onMuteStateChanged` would normally
fire the `applyMuteStateFromAppNotif` callback, which would chime *again* — that
caused the original "double chime" bug.

Empirically, some apps post 3–5 notification updates within ~50–80 ms while the
mute state settles, even flapping `Mute → Unmute → Mute → Unmute → Unmute`
during a single toggle. The echo guard suppresses **all** mute-driven applies
for 2 s after a stem-press toggle:

```kotlin
private fun applyMuteStateFromAppNotif(muted: Boolean) {
    val age = SystemClock.elapsedRealtime() - lastSelfMuteAtMs
    if (age < selfMuteEchoWindowMs) {
        // echo from our own stem press — skip
        return
    }
    // ... real in-app toggle, sync OS mic + play chime
}
```

Genuine in-app mute toggles (user taps Teams' on-screen mute button) still fire
the chime correctly — they're not within the 2 s window of any stem press.

---

## `CallNotifListener` (Notification Interception)

`CallNotifListener` is a `NotificationListenerService` that watches call
notifications from supported VoIP apps. Watched packages:

```kotlin
WATCHED_PACKAGES = setOf(
    "com.microsoft.teams",
    "com.microsoft.teams.ipphone",
    "com.microsoft.teams2",
    "com.viber.voip",
    "com.whatsapp",
)
```

### Companion-object state

| Field | Purpose |
|---|---|
| `muteAction` / `unmuteAction` / `hangUpAction` / `answerAction` / `declineAction` | Cached `Notification.Action` references resolved by the three-tier pipeline. |
| `muteShownByAppNotif` (was `teamsMuted`) | Canonical mute state — true ⇒ "Unmute" button visible ⇒ mic is muted. |
| `isVoipNotifRinging` | True while a ringing call notification (answer+decline cached) is posted by any watched app. Consumed by `AirPodsService.isInAnyCall()`. |
| `isVoipNotifActive` | True while an active-call notification (mute/hangUp cached) is posted. |
| `onMuteStateChanged` | Callback fired when `muteShownByAppNotif` changes. |
| `lastSeenKey` | Used to detect when the cached notification is removed. |

### Dispatch helpers

`answerCall()`, `declineCall()`, `hangUp()`, `setMuted(muted)` all funnel
through a single helper that applies the BAL bypass:

```kotlin
private fun sendActionPendingIntent(action: Notification.Action, label: String): Boolean {
    val ctx = ServiceManager.getService()?.applicationContext
    val fillIn = Intent().apply { addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) }
    val optsBundle = if (Build.VERSION.SDK_INT >= 34) {
        ActivityOptions.makeBasic()
            .setPendingIntentBackgroundActivityStartMode(
                ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED
            )
            .toBundle()
    } else null
    action.actionIntent.send(ctx, 0, fillIn, null, null, null, optsBundle)
    return true
}
```

---

## Stem Config — `setupStemActions()`

The AirPods firmware resets its press-forwarding config when entering HFP call
state. We re-apply our "report all presses to the app" config when:

| Trigger | Why |
|---|---|
| `TelephonyManager.CALL_STATE_RINGING` | Firmware resets stem config on HFP call state change |
| `TelephonyManager.CALL_STATE_OFFHOOK` | Re-apply for active call |
| `CallNotifListener` detects VoIP ringing | Self-managed VoIP doesn't trigger `CALL_STATE_RINGING` |
| `onStemPressReceived()` when `isInAnyCall()` | Safety net |

The AACP `STEM_CONFIG` (identifier `0x39`) is sent with a bitmask:
- `0x07` — bits 0,1,2 (single + double + triple)
- `0x0F` — bits 0,1,2,3 (single + double + triple + long, when gym mode or
  custom long-press configured)

---

## Confirmation Chime

`GestureFeedback.playConfirmation(isYes)` plays the same `confirm_yes` /
`confirm_no` sample used for head gestures. To remain audible during a call,
the chime is **stacked 4×** — `SoundPool.play(...)` invoked 4 times in
succession with the same sample, summing to roughly +12 dB (≈3× perceived
loudness).

`SoundPool` is configured with `MaxStreams = 6` (allowing the 4 chime
overlaps plus headroom for directional + mute-reminder sounds), AudioAttributes
`USAGE_ASSISTANCE_ACCESSIBILITY` + `CONTENT_TYPE_SONIFICATION` +
`FLAG_AUDIBILITY_ENFORCED`.

---

## Debugging

Key log tags for diagnostics:

```bash
adb -s <serial> logcat -v time | grep -E \
  "onStemPressReceived|handleCallStemPress|isInAnyCall|answerCall|rejectCall|\
   dispatchHeadsethookToTelecom|CallNotifListener|setupStemActions|toggleMicMute|\
   applyMuteStateFromAppNotif|VoipCallMonitor"
```

Expected sequence — Viber ringing → answer → mute → end:

```
CallNotifListener: Scanning com.viber.voip notification (..., actions=[Decline, Answer])
CallNotifListener: Cached actions from com.viber.voip (tier1Hit=true callType=1): ...
CallNotifListener: isVoipNotifRinging: false → true (com.viber.voip)
CallNotifListener: Ringing VoIP call detected for com.viber.voip — calling setupStemActions()
AirPodsService: isInAnyCall: true (CallNotifListener: ringing=true active=false)
AirPodsService: onStemPressReceived: type=SINGLE_PRESS
AirPodsService: handleCallStemPress: SINGLE_PRESS → answerCall() (voip=false, notif=true)
AirPodsService: dispatchHeadsethookToTelecom: no Telecom session in active sessions
AirPodsService: answerCall: TelecomManager.acceptRingingCall() called
ActivityTaskManager: START ACCEPT_CALL ... BAL_ALLOW_TOKEN [realCaller] result code=0
CallNotifListener: answerCall: fired Answer

AirPodsService: onStemPressReceived: type=SINGLE_PRESS  (active state)
AirPodsService: handleCallStemPress: isMutePress=true
AirPodsService: toggleMicMute: setMicrophoneMute(true)
CallNotifListener: setMuted(true): fired Mute
GestureFeedback: Playing NO confirmation x4 - streamIDs=...
CallNotifListener: App-notif mute state changed → true (com.viber.voip)
AirPodsService: applyMuteStateFromAppNotif: echo from our own stem press — skipping

AirPodsService: onStemPressReceived: type=DOUBLE_PRESS
AirPodsService: handleCallStemPress: isEndCallPress=true → rejectCall()
AirPodsService: dispatchHeadsethookToTelecom: dispatched DOWN=true UP=true
CallNotifListener: Call notification removed; clearing cached actions
```

---

## Known limitations

1. **Microsoft Teams meeting active-call notification** uses `BigTextStyle` (no
   `CallStyle` extras). Its Mute / Hang up actions are resolved by tier-3
   keyword matching only. Exotic locales not in our keyword list (Indonesian,
   Vietnamese, Hungarian, Czech, …) will silently fail to cache the mute /
   hangUp actions for Teams meetings. Workaround: stem presses to mute/end an
   active Teams meeting will fall through to `AudioManager.setMicrophoneMute`
   (mute works) or `dispatchHeadsethookToTelecom` (end works because Teams
   meetings are Telecom-registered). The visible Teams in-app UI may not
   reflect the mute state.

2. **No `Notification.CallStyle` mute extra exists in the AOSP framework.**
   Mute resolution for ALL apps is title-keyword based. We cover 15+ locales;
   exotic ones may not work.

3. **Stacked-chime ceiling.** 4 simultaneous `SoundPool.play` invocations on the
   same sample sum to ~+12 dB. Beyond that, digital clipping becomes audible.
   If a user has the call volume turned very low, the chime may still be
   inaudible — the OS volume is a hard upper bound.

---

## Test harness reference

The discovery of the BAL bypass came from a 10-method investigation harness
preserved on the `test/viber-answer-methods` branch. The harness fires a burst
of candidate answer methods on a single stem press and logs which one
successfully answered. Methods tried:

| # | Method | Outcome |
|---|---|---|
| 0 | `PendingIntent.send()` no flags | BAL_BLOCK |
| 1 | `PendingIntent.send(ctx, 0, fillIn[FLAG_ACTIVITY_NEW_TASK])` | BAL_BLOCK |
| 2 | `PendingIntent.send(...)` with `ActivityOptions.setPendingIntentBackgroundActivityStartMode(ALLOWED)` | **BAL_ALLOW_TOKEN — winner** |
| 3 | `TelecomManager.acceptRingingCall()` | Silently fails (Viber not in Telecom) |
| 4 | `dispatchHeadsethookToTelecom()` (WhatsApp winner) | No Telecom session for Viber |
| 5 | `AudioManager.dispatchMediaKeyEvent(KEYCODE_HEADSETHOOK)` | No-op |
| 6 | HEADSETHOOK to ALL active MediaSessions | No active session listens for it |
| 7 | KEYCODE_CALL to ALL active MediaSessions | No-op |
| 8 | Reflection — extract Intent from PendingIntent → send directly | Hidden API blocked |
| 9 | Accessibility tap on "Answer" button | Works but requires extra setup |

Method 2 is now the production answer path inside `CallNotifListener.answerCall`.
