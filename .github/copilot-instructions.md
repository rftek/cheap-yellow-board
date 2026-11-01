# XBOX Controller Test

- PlatformIO project using native ESP-IDF C for an ESP32 Cheap Yellow Display. Each application has its own branch: `bootloader`, `wifi_checker`, `birdnet_viewer`, `screen_check`, `touchscreen_cal`, `stopwatch`, `gear_roll`, `Datalogger`, `CatMouse`, `bluetooth_diag`, `f1_race`, `xbox_controller` (this); docs on `How_to_Build`.
- Pairs with an Xbox wireless controller over Bluetooth with [Bluepad32](https://github.com/ricardoquesada/bluepad32), then shows a live test of every button, D-pad direction, stick and trigger.
- Bluepad32 runs on the **BTstack** host (not Bluedroid/NimBLE): `sdkconfig.defaults` sets `CONFIG_BT_CONTROLLER_ONLY=y`. Bluepad32 + BTstack are vendored as a submodule at `external/bluepad32`; the CI workflow inits it recursively, applies Bluepad32's BTstack patch and runs BTstack's `integrate_btstack.py`.
- `btstack_run_loop_execute()` in `app_main` never returns, so the Bluepad32 callbacks run on that task and the UI runs on a second task. State is shared through one struct guarded by a `portMUX` spinlock.
- The custom platform is `get_xbox_platform()`; `on_init_complete` enables scanning and incoming connections. "Forget & re-pair" sets an NVS flag and reboots so `uni_bt_del_keys_unsafe()` can run on the BTstack task.
- `components/cyd_display` is copied on every application branch; keep the copies in sync.
- No Wi-Fi or credentials. The binary also runs as a `bootloader` launcher image; keep it under 1.31 MB.
- Build with `pio run`; verify firmware changes with a PlatformIO build before flashing.
