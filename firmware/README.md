# FishWatch 养鱼手表 — firmware (v0.7.9)

ESP-IDF firmware for the **Waveshare ESP32-S3-Touch-AMOLED-2.06** watch.
The phone companion page is the `index.html` at the repo root (served by GitHub Pages).

## Hardware
410×502 AMOLED (SH8601, QSPI), FT5x06 touch, ESP32-S3R8 (8 MB PSRAM), 32 MB flash,
QMI8658 IMU (0x6B/0x6A), PCF85063 RTC (0x51, stores UTC), AXP2101 PMU (0x34),
ES8311 speaker codec, ES7210 mic codec, TF slot, vibration motor GPIO18,
PWR button GPIO10 (active high), BOOT GPIO0 (active low).

## Build
ESP-IDF **v5.5.x**. All dependencies are vendored in `components/` — the component
manager is not used.

```
export IDF_COMPONENT_MANAGER=0
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash          # or merge: cd build && python -m esptool --chip esp32s3 merge-bin -o fishwatch_merged.bin @flash_args  (flash at 0x0)
```

Vendored: Waveshare BSP (from the factory example), esp_lcd_sh8601, esp_lcd_touch(+ft5x06),
esp_lvgl_port, esp_codec_dev (from esp-adf), LVGL **v9.5.0** (its `scripts/` folder is omitted; not needed to build).

## Layout (`main/`)
| file | what |
|---|---|
| `main.c` | startup, crash guard (≥2 failed boots → safe mode), stable task |
| `app.h` | `settings_t` (NVS blob) + all module APIs |
| `store.c` | NVS settings (tolerates older shorter blobs), SD card, workout CSV |
| `hw.c` | PMU/RTC/IMU over I2C, motor, buttons, step counter, raise-to-wake |
| `power.c` | screen timeout/sleep/wake, 500 ms tick (steps, reminders, alarm, weather, NTP), DFS |
| `net.c` | on-demand WiFi (refcount `net_wifi_use`, 20 s idle off), up to 6 saved networks, NTP, Open-Meteo weather |
| `ble.c` | NimBLE, started only when user taps pairing; auto-off after 3 min idle |
| `health.c` | 30-day steps, hourly steps, workout sets, sleep movement; export to phone |
| `sound.c` | ES8311 synthesized sounds (24 kHz) |
| `ui_core.c` | helpers, navigation, toast |
| `ui_tank.c` | fish-tank watch face (time-based fish sim, feed = double tap, rub = clean) |
| `ui_apps.c` | honeycomb app menu (20 apps, category colors) |
| `ui_control.c` | control center |
| `app_workout.c` | barbell workout log |
| `app_misc.c` / `app_more.c` / `app_extra.c` | notes, phone pairing (QR), faces, flashlight, stopwatch, find phone, settings, steps, focus, alarm, recorder, weather, office light (Supabase RPC), tilt game, reminders |
| `assets/`, `fonts/` | generated C arrays — regenerate with `tools/gen_assets.py` and `tools/gen_fonts.sh` |

Fonts: Inter SemiBold (time/numbers), Noto Sans SC Medium (CJK subset extracted from `main/*.c`), uncompressed.
Source font files are not in the repo (large); `gen_fonts.sh` expects them in `tools/fonts/`.
When adding new Chinese text, rerun `gen_fonts.sh` or the new glyphs show as boxes.

## BLE protocol
Service `f15a000X-6c2b-4b9a-9d3e-2a1c5e7b8d00`, X = 
2 AUTH (pairing code), 3 TIME, 4 WIFI, 5 STATUS (notify JSON), 6 CMD (notify),
7 LOC, 8 DATA (notify JSON lines, ends with `END`).
CMD strings: `find_stop`, `buzz`, `sync`, `wifi_del:<ssid>`, `goal:<n>`, `url:<page>`,
`lightkey:<key>`, `remind:XY`, `wx:t,code,max,min,hum`, `dump` (stream the crash dump over DATA),
`dump_clear`, `lightacct:<person uuid>\n<pin>` / `lightout` (office light by mr-tracker account, 0.7.8+).
Watch → phone on CMD: `find`, `find_stop`, `wifi_ok:<ssid>`, `wifi_fail:<ssid>\n<reason>`.
WIFI writes are tried first and only saved if the watch gets an IP (since 0.7.6).
STATUS JSON also carries `rst` (last reset reason) and `dump` (a core dump is stored) since 0.7.7.

## Crash diagnostics (0.7.7+)
A panic, an LVGL assert (`components/fw_lv_assert.h` → abort) or the task watchdog (the UI task
is watched too, fed by the 500 ms power tick) reboots the watch and writes an ELF core dump to
the `coredump` partition. On the next boot the watch toasts the reason; the phone page shows it
under 手表状态 and can download the dump. Decode it with the matching ELF from `releases/`:
```
xz -dk releases/fishwatch_v0.7.7.elf.xz
python -m esp_coredump info_corefile -t raw -c fishwatch-coredump-....bin releases/fishwatch_v0.7.7.elf
```
Chrome Web Bluetooth allows one GATT operation at a time — the phone page queues them.

## Office light
Since 0.7.8 `app_extra.c` calls Supabase RPC `light_set(p_person_id, p_pin, ch, new_state)` over HTTPS
(esp_crt_bundle) with the user's mr-tracker account. The phone page checks email + PIN with
`login_by_email` first and only then sends `lightacct:`; on a `not signed in` reply the watch
forgets the PIN at once (5 wrong PINs lock the account). The old per-device key (`lightkey:`,
`device_set_light`) is no longer used by the phone page. Open issue: as of v0.7.1 the watch's requests never reach Supabase
(weather over plain HTTP works) — v0.7.1 shows the exact error under the buttons to diagnose.
