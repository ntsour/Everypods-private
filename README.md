<p align="center">
  <img src="https://img.shields.io/github/stars/ntsour/Everypods?style=for-the-badge&logoColor=white" />
  <img src="https://img.shields.io/github/license/ntsour/Everypods?style=for-the-badge" />
  <img src="https://img.shields.io/github/v/release/ntsour/Everypods?style=for-the-badge&logoColor=white&label=Release" />
  <img src="https://img.shields.io/github/downloads/ntsour/Everypods/total?style=for-the-badge&label=Downloads" />
  <img src="https://img.shields.io/github/issues/ntsour/Everypods?style=for-the-badge" />
</p>

![EveryPods Banner](./imgs/banner.png)

# What is EveryPods?

EveryPods unlocks Apple's exclusive AirPods features on non-Apple devices. Get access to noise control modes, adaptive transparency, ear detection, battery status, and more - all the premium features you paid for but Apple locked to their ecosystem.

# Device Compatibility

| Status | Device                | Features                                                   |
| ------ | --------------------- | ---------------------------------------------------------- |
| ✅      | AirPods Pro (2nd Gen) | Fully supported and tested                                 |
| ✅      | AirPods Pro (3rd Gen) | Fully supported (except heartrate monitoring)              |
| ✅      | AirPods Max           | Fully supported (client shows unsupported features)        |
| ⚠️      | Other AirPods models  | Basic features (battery status, ear detection) should work |

Most features should work with any AirPods. Currently, I've only got AirPods Pro 2 to test with. But, I believe the protocol remains the same for all other AirPods (based on analysis of the bluetooth stack on macOS).

# Key Features

- **Noise Control Modes**: Easily switch between noise control modes without having to reach out to your AirPods to long press
- **Ear Detection**: Controls your music automatically when you put your AirPods in or take them out, and switch to phone speaker when you take them out
- **Battery Status**: Accurate battery levels
- **Head Gestures**: Answer calls just by nodding your head
- **Conversational Awareness**: Volume automatically lowers when you speak
- **Multi-device connectivity** (upto 2 devices)
- **Other customizations**:
  - Rename your AirPods
  - Customize long-press actions
  - All accessibility settings
  - And more!

# Platform Support

## Linux
for the old version see the [Linux README](./linux/README.md). (doesn't have many features, maintainer didn't have time to work on it)

new version in development ([#241](https://github.com/ntsour/Everypods/pull/241))

![new version](https://github.com/user-attachments/assets/86b3c871-89a8-4e49-861a-5119de1e1d28)

## Android

### Screenshots

|                                                                                         |                                                    |                                                                              |
| --------------------------------------------------------------------------------------- | -------------------------------------------------- | ---------------------------------------------------------------------------- |
| ![Settings 1](./android/imgs/settings-1.png)                                            | ![Settings 2](./android/imgs/settings-2.png)       | ![Debug Screen](./android/imgs/debug.png)                                    |
| ![Battery Notification and QS Tile for NC Mode](./android/imgs/notification-and-qs.png) | ![Popup](./android/imgs/popup.png)                 | ![Head Tracking and Gestures](./android/imgs/head-tracking-and-gestures.png) |
| ![Long Press Configuration](./android/imgs/long-press.png)                              | ![Widget](./android/imgs/widget.png)               | ![Customizations 1](./android/imgs/customizations-1.png)                     |
| ![Customizations 2](./android/imgs/customizations-2.png)                                | ![accessibility](./android/imgs/accessibility.png) |                                                                                |


here's a very unprofessional demo video

https://github.com/user-attachments/assets/43911243-0576-4093-8c55-89c1db5ea533

### Device Requirement

EveryPods does not require root. Full AACP control (noise control modes, accessibility settings, and more) needs a device where the Android Bluetooth stack supports opening an L2CAP channel to the AirPods:

- **Android 17 (SDK 37)** and above: supported on all OEMs.
- **Android 16 (SDK 36)**: supported on Google Pixel and OnePlus/OPPO devices.
- Other devices fall back to basic features (battery status, ear detection, standard Bluetooth audio) until their OEM ships the fix. [This issue is being tracked here](https://issuetracker.google.com/issues/371713238). Please do not comment on the issue thread.

### A few notes

- Due to recent AirPods' firmware upgrades, you must enable `Off listening mode` to switch to `Off`. This is because in this mode, loud sounds are not reduced.

- If you have take both AirPods out, the app will automatically switch to the phone speaker. But, Android might keep on trying to connect to the AirPods because the phone is still connected to them, just the A2DP profile is not connected. The app tries to disconnect the A2DP profile as soon as it detects that Android has connected again if they're not in the ear.

- When renaming your AirPods through the app, you'll need to re-pair them with your phone for the name change to take effect. This is a limitation of how Bluetooth device naming works on Android.

## Multi-device Connectivity

Upto two devices can be simultaneously connected to AirPods, for audio and control both. Seamless connection switching. The same notification shows up on Apple device when Android takes over the AirPods as if it were an Apple device ("Move to iPhone"). Android also shows a popup when the other device takes over.

# Supporters

A huge thank you to everyone supporting the project!
- @davdroman
- @tedsalmon
- @wiless
- @SmartMsg
- @lunaroyster
- @ressiwage

# Special thanks
- @tyalie for making the first documentation on the protocol! ([tyalie/AAP-Protocol-Definition](https://github.com/tyalie/AAP-Protocol-Defintion))
- @rithvikvibhu and folks over at lagrangepoint for protocol research ([gist](https://gist.github.com/rithvikvibhu/45e24bbe5ade30125f152383daf07016))
- @devnoname120 for early Bluetooth-stack research
- @timgromeyer for making the first version of the linux app
- @hackclub for hosting [High Seas](https://highseas.hackclub.com) and [Low Skies](https://low-skies.hackclub.com)!

# Alternates for other platforms:
- CAPod - A companion app for AirPods on Android. ([play store](https://play.google.com/store/apps/details?id=eu.darken.capod) | [source code](https://github.com/d4rken-org/capod))
- MagicPods for Steam Deck ([website](https://magicpods.app/steamdeck/))
- MagicPods - if you're looking for "EveryPods for Windows"  ([ms store](https://apps.microsoft.com/store/detail/9P6SKKFKSHKM) [installer](https://magicpods.app/installer/MagicPods.appinstaller) | [website](https://magicpods.app/))

# Nightly/Development Builds

Want to try the latest features before they're officially released? You can grab nightly builds from GitHub Actions:

### Android
1. Go to the [Actions tab](https://github.com/ntsour/Everypods/actions/workflows/ci-android.yml)
2. Click on the most recent successful workflow run
3. Scroll down to **Artifacts** and download the **Debug APK** zip file
4. Extract the zip and install the `.apk` on your device

> [!NOTE]
> You need to be signed in to GitHub to download artifacts. Nightly builds are debug-signed and may not auto-update. You may need to uninstall the stable version first.

### Linux (Rust)
1. Go to the [Actions tab](https://github.com/ntsour/Everypods/actions/workflows/ci-linux-rust.yml)
2. Click on the most recent successful workflow run
3. Download the Linux binary from **Artifacts**

> [!WARNING]
> Nightly builds are unstable and may contain bugs. Use at your own risk.

# Star History

<a href="https://www.star-history.com/#ntsour/Everypods&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=ntsour/Everypods&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=ntsour/Everypods&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=ntsour/Everypods&type=date&legend=top-left" />
 </picture>
</a>

# License

EveryPods - AirPods liberated from Apple’s ecosystem
Copyright (C) 2025 EveryPods contributors

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

All trademarks, logos, and brand names are the property of their respective owners. Use of them does not imply any affiliation with or endorsement by them. All AirPods images, symbols, and the SF Pro font are the property of Apple Inc.
