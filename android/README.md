# FishWatch 通知 (Android)

Forwards phone notifications to the watch over BLE (watch firmware 0.8.0+, 通知 → 收通知 on).
The APK is built by GitHub Actions (`.github/workflows/android-notify.yml`) and attached to the
`notify-app` release. `app/debug.keystore` is a throwaway debug key kept in the repo on purpose so
every build installs over the previous one.

Protocol: same GATT service as the phone page (`f15a000X-6c2b-4b9a-9d3e-2a1c5e7b8d00`): write the
pairing code to 0002 (AUTH), then `app\ntitle\ntext` (UTF-8, ≤ 500 bytes) to 0009 (NOTIF).
