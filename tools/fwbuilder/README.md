# Firmware builder

A small web app for building CircuitPython from this fork for the boards of
the espressif, raspberrypi, atmel-samd and nordic ports: pick a board, switch
modules on and off, keep the choices as named profiles, build, and keep,
download, flash or delete the resulting firmware.

## Running

From WSL, in the repository:

    python3 tools/fwbuilder/server.py

then open <http://localhost:8080> in the Windows browser (WSL 2 forwards
localhost). Options: `--port 8080`, `--host 127.0.0.1`.

Only the Python standard library is needed in WSL. The toolchains are the ones
the ports build with anyway:
- espressif: the ESP-IDF in `ports/espressif/esp-idf`, set up as for any build;
- raspberrypi, atmel-samd, nordic: the Arm GNU toolchain in `FWB_ARM_BIN`,
  put first on the PATH of the build, so that it wins over the older Ubuntu
  package (13.2) in `/usr/bin`. The builds were tried with GCC 14 from
  `/opt/arm-gcc14`; CircuitPython's CI uses 15.3.Rel1. The port submodules have
  to be fetched
  (`make -C ports/<port> fetch-port-submodules`);
- nordic also runs `hexmerge.py` from `intelhex`, which `pip install --user`
  puts in `~/.local/bin`. That is on the PATH of a login shell, so start the
  server from a normal WSL terminal.

Flashing, the list of COM ports and the UF2 drives go through the Windows
Python, which sees what WSL does not. It needs Python 3.12 or newer
(`os.listdrives`) with `esptool` and `pyserial` installed (`py -m pip install
esptool pyserial`).

| Environment variable | Default | Meaning |
|---|---|---|
| `FWB_DATA` | `~/cp-fwbuilder` | profiles and kept firmware |
| `FWB_WIN_PYTHON` | `/mnt/c/Python312/python.exe` | the Windows Python used for esptool, the serial ports and the UF2 drives |
| `FWB_ARM_BIN` | `/opt/arm-gcc14/bin` | the Arm toolchain for the ports other than espressif |

## What it does

**Language.** The page is in Czech or English. It follows the browser
(Czech or Slovak means Czech) until a language is picked at the foot of the
rail; the choice is kept in the browser's local storage. The texts are in
`static/i18n.js`.

**Sestavení (build).**
- Lists the boards in `ports/<port>/boards` of the four ports (about 600),
  with a search box and a filter grouped by port: a whole port, or one chip
  family (S3, C5, RP2040, RP2350, SAMD21, SAMD51, NRF52840, …). The chosen
  filter is remembered. The chip comes from `IDF_TARGET` on espressif,
  `CHIP_VARIANT` on raspberrypi, `CHIP_FAMILY`/`CHIP_VARIANT` on atmel-samd and
  `MCU_CHIP` on nordic. Board directory names do not repeat across these ports,
  so a board's id is still its directory name and older profiles and firmware
  keep working.
- The other ports in `ports/` (stm, mimxrt10xx, zephyr-cp, silabs, analog,
  cxd56) are left out: they are not used here, and some need SDKs that are not
  set up.
- For the chosen board it asks make for the value of every `CIRCUITPY_*`
  variable (`make BOARD=… --eval=…`, about half a second). The board's own
  defaults are therefore what the build files actually say.
- The 0/1 switches are shown in three groups: the fork's additions (with
  descriptions), modules (those with a `shared-bindings` or port `bindings`
  directory), and other options, which are hidden until asked for.
- Whatever differs from the board's default is passed to make on the command
  line (`CIRCUITPY_X=0/1`) and listed in the drawer.
- A profile stores the board and these differences.
- Fork additions the board cannot have are hidden; "show unavailable" lists
  them greyed out with the reason. The reasons are worked out, not listed by
  hand, and only on espressif: from `fork.mk` (an `$(error)` under an `IDF_TARGET` test limits a
  switch to that chip, one under a `,0` test or a default taken from other
  switches makes it need them, and a per-chip path such as a prebuilt library
  must exist for the chip), and from the chip's `soc_caps.h` in the IDF for the
  hardware each switch drives (`SOC_REQUIRED` in `server.py`). Turning off a
  switch that others need turns those off too.
- The "i" next to each switch shows what it enables and what it is for. The
  texts, in Czech and English, are in `descriptions.json`, keyed by the name
  without `CIRCUITPY_`. Where the source has more to say, the tooltip adds the
  first paragraph of the module's docstring or the comment above the variable
  in the `.mk` files.

**Build.**
- Runs `make BOARD=… BUILD=build-fwb-<board>-<hash> <switches> -j$(nproc)` in
  the background and streams the output into the Log view.
- The build directory is named after the board and the switches. The same
  combination therefore builds incrementally, and a new one starts in a clean
  directory. Changing the set of modules in an existing directory leaves stale
  generated headers behind.
- "Clean build" deletes that build directory first, for when the same
  combination has to be built from scratch (after changes to sdkconfig, say).
- One job runs at a time and can be cancelled.

**Firmware.**
- After a successful build, `firmware.bin`, and `firmware.uf2` for boards with
  the UF2 bootloader (every board of the ARM ports), are copied to
  `FWB_DATA/firmware/<id>/`.
- The `manifest.json` written with them holds: board, port, chip, profile, time,
  `git describe`, the switches changed, every option enabled, and the flash used
  and free from the build log.
- The list shows these. Each entry can be downloaded (BIN or UF2), flashed or
  deleted.
- Deleting removes the build directory too, unless another kept firmware came
  from it.

**Flash, espressif.**
- Writes `firmware.bin` at 0x0 with
  `esptool --chip <chip> -p COMx --after watchdog-reset write-flash`.
- The full flash can be erased first, which also erases the files on the board.
- Chips that do not reset into the bootloader by themselves (the S3 running
  TinyUSB) have to be put in download mode by hand: hold BOOT, press RESET.

**Flash, the other ports.**
- Copies `firmware.uf2` to the bootloader's drive, found by its
  `INFO_UF2.TXT`. The dialog lists the drives with their `Board-ID`; a single
  one is chosen by default.
- Without a drive, the board on the chosen COM port is sent to its bootloader
  first. The port is opened at 1200 baud and closed, which makes CircuitPython
  call `reset_to_bootloader()`. The new drive is then waited for up to 20 s.
  That works on RP2040/RP2350 (`reset_usb_boot`) and SAMD (the double-tap
  magic).
- On nRF, CircuitPython asks the bootloader for its serial-only DFU
  (`BOOTLOADER_DFU_MAGIC` 0x4e), which shows no drive. These boards have to be
  put in the bootloader by hand (double-press RESET), and the drive chosen.
- On espressif the same 1200 baud touch goes to the ROM download mode, not
  TinyUF2, which is why espressif keeps esptool.

**Board info.** The button after the COM port reads the board on that port (ESP32 only),
read only: `esptool flash-id` for the chip, revision, features, crystal, MAC and
flash size, and `espefuse summary` for the PSRAM and flash capacity codes and
the package. esptool decodes the PSRAM size only for the S3; other chips show
the raw eFuse code. The internal SRAM comes from the chip family's datasheet.
The board restarts afterwards.

## API

| Method and path | |
|---|---|
| `GET /api/boards` | boards: id, name, port, chip, target |
| `GET /api/boards/<id>/modules` | port, chip, UF2, and every switch with its default and group |
| `GET/POST /api/profiles`, `DELETE /api/profiles/<name>` | named profiles |
| `POST /api/build` `{board, overrides, profile, clean}` | starts a build; `clean` deletes the build directory first |
| `POST /api/flash` `{firmware, port, drive, erase}` | starts flashing; `drive` (`E:`) is for UF2 firmware, without it the board on `port` is sent to its bootloader |
| `GET /api/uf2drives` | the UF2 bootloader drives and their `Board-ID` |
| `GET /api/job`, `GET /api/job/stream`, `POST /api/job/cancel` | the running job; the stream is server-sent events |
| `GET /api/firmwares`, `DELETE /api/firmwares/<id>` | kept firmware |
| `GET /firmware/<id>/firmware.bin` (or `.uf2`) | download |
| `GET /api/ports` | COM ports with their descriptions |
| `GET /api/probe?port=COMx` | chip, flash, PSRAM and more read from the board |

## Notes

- Switching on a module the chip does not have fails the build. The log shows
  where.
- The S3 boards' 2 MB app partition fits only some of the fork's additions at
  once. The flash bar in the firmware list shows how close a build came.
- The server listens on localhost only and has no authentication; it runs
  `make` and `esptool` with what the page sends.
- A build of a port other than espressif checks only that make accepts the
  switches. What the page turns on is not checked against the chip; the build
  log shows where it fails.
