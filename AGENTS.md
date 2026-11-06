# Agent notes: build and flash

ESP-IDF firmware for the Waveshare ESP32-S3-Touch-LCD-4B. The toolchain is already
installed on this machine. Do not reinstall it, and do not use `eim`.

## Toolchain

| Item | Location |
|---|---|
| ESP-IDF v5.4.2 (matches `dependencies.lock`) | `~/Work/esp/v5.4.2/esp-idf` |
| Compilers, Python venv | `~/Work/esp/.espressif` (`python_env/idf5.4_py3.13_env`) |
| Activation script | `~/Work/esp/idf54.sh` |
| ninja, dfu-util, ccache, cmake | Homebrew |

## Activate

Every shell command starts in a fresh environment, so activate in the same command
that uses `idf.py`:

```bash
. ~/Work/esp/idf54.sh >/dev/null 2>&1 && cd ~/Work/piano-practice-monitor && idf.py build
```

- Do not pipe the activation (`. ~/Work/esp/idf54.sh | tail`). That sources it in a subshell
  and `idf.py` is then "command not found". Redirect its output instead.
- Do not source `export.sh` directly. `python3` on PATH is system 3.9.6, so it will not
  find the 3.13 venv. `idf54.sh` puts Homebrew Python 3.13 first.

## Build

```bash
idf.py build                  # incremental
make -C test                  # host tests, no IDF needed
```

Output is `build/practice_logger.bin`.

`sdkconfig` is generated and gitignored. It only picks up new `sdkconfig.defaults`
values when regenerated, so after editing the defaults run:

```bash
rm -f sdkconfig && idf.py set-target esp32s3 && idf.py build
```

`set-target` wipes `sdkconfig`, including Wi-Fi settings entered in menuconfig. Never put
Wi-Fi credentials in `sdkconfig.defaults`, which is tracked.

## Do not flash without a backup

The board is not running this project. As of 2026-11-04 it runs `piano_monitor` v0.1.1
(built 2026-10-25) from `factory` at `0x100000`, with `xiaozhi` 1.8.5 in `ota_0`, and a
different partition table (`storage` at `0xc00000`). Flashing this project replaces that
table and overwrites the start of `piano_monitor`, and the old `storage` becomes
unreachable. Take a full flash backup and get the user's approval first.

Wi-Fi here is compile-time only (`CONFIG_PL_WIFI_SSID`). Credentials saved by the
firmware on the board are not reused, so a build with an empty SSID runs offline.

A second copy of this repo with uncommitted changes is at
`~/Work/Codex/2026-10-19/referenced-chatgpt-conversation-this-is-an-2/piano-practice-monitor`.
Those changes are not in this copy.

## Flash

The board enumerates as `/dev/cu.usbmodem1101` (Espressif USB-Serial/JTAG). Check with
`ls /dev/cu.usbmodem*`, as the number can change.

```bash
esptool.py --port /dev/cu.usbmodem1101 flash_id     # read-only connection check
idf.py -p /dev/cu.usbmodem1101 flash                # overwrites the board: ask the user first
idf.py -p /dev/cu.usbmodem1101 monitor              # needs a real TTY; Ctrl-] exits
```

Board as detected: ESP32-S3 rev v0.2, 8 MB embedded PSRAM, 16 MB flash. The project
deliberately configures 8 MB flash, so `esptool` reporting 16 MB is not a problem.

## Troubleshooting

- `eim` reports `[FAIL] SSL/HTTPS: could not start local TLS test server`: ignore it and
  do not try to fix Python. Both Pythons do HTTPS correctly. EIM still lists v5.4.2 as
  "in progress"; that entry is stale.
- A sandbox may block network access (the first build downloads `managed_components/`
  from the component registry) or `/dev/cu.*`. If so, ask for elevated permissions rather
  than changing the toolchain.
- `main` builds with `-Werror`, including format-truncation. Size `snprintf` buffers
  for the worst case.
- Using a new `lv_font_montserrat_NN` needs `CONFIG_LV_FONT_MONTSERRAT_NN=y` in
  `sdkconfig.defaults`.
