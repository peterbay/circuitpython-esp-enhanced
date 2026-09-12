# Changes in this fork, against adafruit/circuitpython

Every change this fork carries on top of `adafruit/main`, as of the merge of
2026-09-12 (upstream base `10.4.0-alpha.1`). Each entry links to the commit in
[peterbay/circuitpython-esp-enhanced](https://github.com/peterbay/circuitpython-esp-enhanced).

**Not listed here:** the interpreter performance work — the VM dispatch, the
lookup caches, the allocator and growth policies, the IRAM placement, and the
follow-up fixes that belong to it. That is roughly fifty commits and it is
described, with measurements, in `FORK-CHANGES.md` sections 1 and 5. This file
covers new functionality, hardware support, and defect fixes.

`FORK-CHANGES.md` is the narrative companion: why each piece exists, how it was
measured, and what was tried and rejected. This file is the index.

Links are written as `[sha]`; prefix them with
`https://github.com/peterbay/circuitpython-esp-enhanced/commit/`.

---

## 1. New modules and APIs

### `espidf` — the ESP-IDF from Python

[`8ef23454ee`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8ef23454ee)

One ESP-specific module (`ports/espressif/bindings/espidf/`) for the parts of
the ESP-IDF that CircuitPython never exposed. It is a collection rather than one
feature; what follows is what it offers and how each part is used.

**Timers.** `esp_timer` driven from Python. The callback runs between two
bytecodes, not in an interrupt, so it cannot pre-empt running code:

```python
import espidf
t = espidf.Timer(callback, 0.02)              # period in seconds
t = espidf.Timer(callback, 0.5, repeat=False) # one shot
t.stop(); t.start(); t.active; t.deinit()
```

Eight at once; an exception in the callback is printed and the timer keeps
running.

**Flash partitions.** Raw access to the partition table, and `mmap()` returning
a read-only memoryview so a large table can be read with nothing on the heap:

```python
espidf.partitions()             # [(label, type, subtype, address, size), ...]
p = espidf.Partition("ota_1")
p.erase(0, 4096); p.write(0, data); p.read(0, 100)
mv = p.mmap()                   # read-only memoryview, 0 bytes of heap
espidf.running_partition()      # the label writes are refused on
```

`partition_disk` (frozen Python) wraps a partition as a block device for
`storage.VfsFat`, and `espidf.expose_partition("ota_1", path="/logs",
usb_writable=False)` in `boot.py` presents one to the host as a second USB drive
(native block device, `ports/espressif/supervisor/partition_disk.c`).

**Named settings storage.** `espidf.NVS` is the layer under
`microcontroller.nvm`: typed values under names, wear levelled, in a namespace
of your choosing.

```python
nvs = espidf.NVS("settings")
nvs["brightness"] = 128;  nvs["ssid"] = "home";  nvs["cal"] = b"\x00\x01"
nvs["brightness"]; nvs.keys(); del nvs["ssid"]; nvs.deinit()
```

**ESP-IDF events.** `espidf.EventQueue(espidf.WIFI_EVENT, espidf.ANY_ID,
size=16)` then `q.get()` returns `(base, event_id, time_us, data)` or `None`.
This is how a board acting as an access point learns that a client joined —
those event bodies were empty before.

**Wi-Fi extras.** `eap_enable()`/`eap_disable()` for WPA2/WPA3 Enterprise
(PEAP/TTLS/TLS credentials, then an ordinary `wifi.radio.connect(ssid)`);
`smartconfig_start()`/`smartconfig_result()`/`smartconfig_stop()` to receive
credentials from a phone; `set_vendor_ie()` to put custom data in the beacons an
AP sends; `wifi_raw_tx()` to transmit a raw 802.11 frame (a test tool for
networks you own); `wifi_sleep_min_active_time(ms)` to keep the radio awake
after a packet, which took a polled server's latency from 97 ms to 15 ms
([`a3d39d3559`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/a3d39d3559)).

**Crypto the IDF already had.** `pbkdf2()`, `hmac_sha256()`, `aes_gcm_encrypt()`
and `aes_gcm_decrypt()`, written against PSA. The GCM tag travels inside the
ciphertext so decryption cannot be asked for without it; a failed tag raises
rather than returning plaintext. PBKDF2 of 4096 iterations goes from 4.8 s in
Python to 371 ms.

**Power and diagnostics.** `power_management(min_frequency=80,
max_frequency=240, light_sleep=False)` for DFS; `check_heap()` to catch a heap
overwrite where it happens; `pin_status(13)` to read what a GPIO is really doing
and whether CircuitPython has it claimed; `ble_prefer_phy(4)` to ask NimBLE for
the Coded (long range) PHY; `heap_caps_*`, `get_time_us()`, `task_stats()`.

**Channel state information.** `espidf.CSI` reports, for every received frame,
how the channel distorted it — one complex value per OFDM subcarrier, which
moves when something in the room does.

```python
c = espidf.CSI(queue=32, source=wifi.radio.ap_info.bssid)
n = c.readinto(buf, meta)               # 0 bytes of heap per record
n = c.readinto_amplitude(amp, meta, 4)  # magnitudes into a ulab array
rec = c.packet()                        # a dict, when convenience matters
```

Needs traffic to measure (a ping loop will do) and `CONFIG_ESP_WIFI_CSI_ENABLED`.
802.11ax parts describe CSI with different structures, and both layouts are
handled — an HE-SU record is 490 bytes against 106 for 11g
([`149ef77e8c`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/149ef77e8c)).

**Measuring one operation.** `get_cycle_count()` and `cycles()` expose the CPU
cycle counter, because `time.monotonic_ns()` is built from a 32768 Hz tick and
cannot see anything shorter than 30 µs
([`8295e9e55d`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8295e9e55d)).

### `registers` and `register_map` — register-based drivers

[`e87ca1de16`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/e87ca1de16)

The bit twiddling every I2C/SPI sensor driver does, in C (portable, no ESP
dependency, gated by `CIRCUITPY_REGISTERS`):

```python
import registers
registers.extract(buf, mask, shift, signed=False, lsb_first=True)  # -> int
registers.insert(buf, mask, shift, value, lsb_first=True)          # in place
```

On top of it, `register_map` (frozen Python) turns a datasheet into attributes,
with named values, scale/offset to physical units, read-only fields, opt-in
per-register caching and a `batch()` that writes each touched register once:

```python
from register_map import RegisterMap
imu = RegisterMap.from_i2c(dev, {
    "whoami":     (0x75, 0, 8, {"mode": "r"}),
    "gyro_range": (0x1B, 3, 2, {"values": {"250dps": 0, "500dps": 1}}),
    "temp":       (0x41, 0, 16, {"signed": True, "width": 2, "lsb_first": False,
                                 "scale": 1 / 340, "offset": 36.53, "mode": "r"}),
})
imu.gyro_range = "500dps"
imu.temp                      # -> 23.7, already in degrees C
```

Two fixes followed: a field spanning a whole 64-bit register now sign-extends
([`c2b670f450`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/c2b670f450)),
and a field that fits a small int is returned without touching the heap
([`505f21f474`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/505f21f474)).

### `ieee802154` — the raw 802.15.4 radio

[`367ee328f8`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/367ee328f8)

The radio that sits under Zigbee, Thread and Matter, as bare frames: what goes
out is what you hand it, what comes in is whatever was on the channel. It
carries no protocol, and building or parsing the MAC header is left to Python.
ESP32-C5 and similar; about 14 kB; `CIRCUITPY_IEEE802154=1`.

```python
import ieee802154
r = ieee802154.Radio(channel=26, pan_id=0x1234, short_address=0x0001, queue=16)
r.send(frame, cca=False, timeout=0.3)   # MAC header + payload: no length byte, no CRC
f = r.receive()                 # a record, or None when the queue is empty
f["data"], f["rssi"], f["lqi"]
n = r.readinto(buf)             # the same frame with nothing allocated
r.energy_detect(0.001)          # dBm on this channel, duration in seconds
r.promiscuous = True; r.auto_ack = False
r.tx_power = 9                  # r.tx_powers lists the steps the radio really has
r.callbacks, r.lost             # what the driver handed up, what the queue dropped:
                                # the first things to read when frames go missing
```

Four driver-level defects had to be worked around for it to function at all:
coexistence vetoing every transmission, the radio refusing a second `enable()`,
transmission reading the frame out of PSRAM where DMA cannot see it (every
frame went out with a valid CRC over the wrong bytes), and the driver starting
promiscuous. Examples: `ports/espressif/bindings/ieee802154/examples/`
(`tx.py`, `rx.py`, `monitor.py`, `sniffer.py`).

### `zigbee` — the stack, not the radio

[`e2cfcf07c6`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/e2cfcf07c6)

Coordinator, router and end device from one build, on esp-zigbee-lib 2.0.4
vendored into the tree. The role is chosen at run time; BLE and Wi-Fi stay in
the firmware beside it. `CIRCUITPY_ZIGBEE=1`.

A device that publishes a measurement is an endpoint and a stack:

```python
import zigbee
endpoint = zigbee.Endpoint(1, zigbee.TEMPERATURE_SENSOR)
stack = zigbee.Stack(role=zigbee.END_DEVICE, channels=1 << 20,  # a channel mask
                     endpoints=(endpoint,))
stack.start()                   # nothing may be asked of either before this
endpoint[zigbee.TEMPERATURE_MEASUREMENT, 0x0000] = 2350   # 23.50 degC, in ZCL units
stack.joined, stack.short_address, stack.channel, stack.pan_id
```

The other side asks the questions. Answers arrive later and on another task, so
they are collected rather than returned:

```python
stack.discover(address)                     # -> stack.descriptor(): endpoints,
                                            #    device type, profile, clusters
stack.read(address, endpoint, cluster, (0x0000,), source_endpoint=1)
stack.write(...); stack.configure_report(...); stack.command(...)
r = stack.report()                          # {'kind', 'address', 'endpoint',
                                            #  'cluster', 'attribute', 'value',
                                            #  'status', 'rssi'} or None
e = stack.event()                           # joins, leaves, Trust Center decisions
stack.neighbors(); stack.network_key; stack.open_network(); stack.factory_reset()
```

Getting a joining device admitted needed a fix outside the module:
CircuitPython's shared `sdkconfig.defaults` turns off
`CONFIG_MBEDTLS_GCM_SUPPORT_NON_AES_CIPHER`, and without it every AES-CCM\*
operation fails, so the Trust Center silently refused every join. The board's
sdkconfig turns it back on. Examples:
`ports/espressif/bindings/zigbee/examples/` (`coordinator.py`, `end_device.py`,
`device.py`, `reader.py`, `outlet.py`).

### `usb_net` — a network interface over USB

[`d6d00ef606`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d6d00ef606)

The board appears to the host as an Ethernet adapter (CDC-NCM), hands the host
an address by DHCP, and answers DNS for its own name, so a browser on the host
reaches a server on the board by name with no network in between. Everything is
set from `boot.py`; `CIRCUITPY_USB_NET=1`.

```python
# boot.py
import storage, usb_hid, usb_net
storage.disable_usb_drive(); usb_hid.disable()
usb_net.enable(ipv4_address="192.168.7.1", netmask="255.255.255.0",
               host_ipv4_address="192.168.7.2", hostname="cardputer.home.arpa")
```

```python
# code.py — an ordinary socketpool server, listening on the board's address
import socketpool, wifi
from adafruit_httpserver import Server, Response
server = Server(socketpool.SocketPool(wifi.radio))
server.serve_forever("192.168.7.1", 80)
```

Costs two IN and one OUT endpoint. `shared-bindings/usb_net/examples/usbnet_demo.py`
is a complete page. Windows 11 needed one change in TinyUSB, below.

### `usb_tmc` — a USB488 instrument VISA can talk to

[`81f6e72bb5`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/81f6e72bb5),
examples [`835ff4591b`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/835ff4591b)

The board presents itself as a USB Test and Measurement Class instrument, the
class scopes and meters use, so pyvisa or NI-VISA talks to it as to any SCPI
instrument. One interface, one endpoint pair. `CIRCUITPY_USB_TMC=1`.

```python
# boot.py
import usb_tmc; usb_tmc.enable()

# code.py
while True:
    cmd = usb_tmc.read(timeout=0)        # None when nothing is waiting
    if cmd and cmd.strip().upper() == b"*IDN?":
        usb_tmc.write(b"CircuitPython,Cardputer,0,1.0\n")
```

`read()` blocks only when asked to — `timeout=0` and `in_waiting()` are the
non-blocking ways in. `status_byte()`/`set_status_byte()` are the IEEE 488.2
status byte, with the message-available bit kept by the module.
`examples/usbtmc_demo.py` is a small instrument (`*IDN?`, `*RST`, `*CLS`,
`*ESR?`, `*ESE`, `*OPC?`, measurements, an output, a `SYST:ERR?` queue) and
`examples/visa_client.py` drives it from a PC. Windows has no USBTMC driver:
bind WinUSB with Zadig, or use NI-VISA.

### `hashlib` completed

[`e16ef905c1`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/e16ef905c1)

`hashlib` had `new()` and nothing else, so `from hashlib import sha256` failed
and `adafruit_hashlib` — which imports six names at once — dropped every
algorithm into its pure Python implementations. SHA-256 of 4 kB took 2.2 s
instead of 238 µs. Added: `md5()`, `sha1()`, `sha224()`, `sha256()`, `sha384()`,
`sha512()`, and `hexdigest()`, `copy()`, `block_size`, `name` on the object.
1184 bytes of flash, verified against 131 CPython reference digests.

### Board support: Seeed XIAO ESP32-C5

[`c5aeac1b49`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/c5aeac1b49)

A new build target: single-core RISC-V, Wi-Fi 6, and the first part here with a
5 GHz radio — which is why it exists, since 802.11ax channel estimates only
appear on a Wi-Fi 6 part. Pin tables, sdkconfig, the C5 block in the port
Makefile, and the board itself. Two C5-specific defects came with it:

- **The flash, PSRAM and USB pins must never be reset.** On this part they are
  ordinary GPIOs, and dropping the flash chip select does not stop execution —
  the core runs on out of the instruction cache until the first miss faults, the
  panic handler faults too, and the chip reboots reporting nothing at all.
  [`171cf97655`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/171cf97655)
- **The USB Serial/JTAG console would die and stay dead.** The receive interrupt
  fires on the *arrival* of a packet, not on the FIFO being non-empty, so a byte
  could be stranded in the endpoint with no interrupt left to come. The FIFO is
  now polled from the background task and from the idle path.
  [`d73026c132`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d73026c132)

### Development tooling: the profiler

[`1b2321c632`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/1b2321c632),
[`4f92960c12`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/4f92960c12)

Built only with `make CIRCUITPY_PROF=1`: cycle-counting probes through the
display refresh and the interpreter's C paths, per-opcode cycle accounting, and
a sampling profiler that reads the program counter of the CircuitPython task
from another task. Read from Python with `espidf.prof_stats()`,
`espidf.prof_opcodes()`, `espidf.profiler_start()/stop()/data()`. The probes
cost about 1.5 ms per frame, so shipping firmware is built without them.

---

## 2. USB

- **Windows accepts the NCM interface.** Windows 11's `UsbNcm.sys` refuses a
  device whose NTB parameters report `wNdpOutDivisor` below 4 (Code 10,
  `STATUS_DEVICE_FEATURE_NOT_SUPPORTED`) and TinyUSB reports 1. The submodule
  moves to `peterbay/tinyusb`, branch `circuitpython-ncm-divisor`, which is
  upstream plus that one line. Linux is unchanged.
  [`8db066a291`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8db066a291)
- **`usb_audio` headset and speaker actually work.** The headset took two
  endpoint numbers, so on the ESP32-S3 (IN endpoints 0 to 4) its microphone sat
  on IN 5: every transfer reported complete and nothing reached the host. The
  speaker dropped 92 % of what the host sent, because nothing scheduled the task
  that drains TinyUSB's OUT FIFO. Windows refused the function outright until
  the OUT endpoint was declared adaptive rather than asynchronous. Host mute and
  volume were stored and ignored; they now scale the samples, per feature unit,
  over a −60…0 dB range.
  [`d9d8eaa050`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d9d8eaa050)
- **`usb_hid` no longer writes through a null pointer.** A report on the
  interrupt OUT endpoint arrives with `report_id` 0, and a test that started
  `report_id == 0 ||` made the copy run with no device found — a write through
  null at an arbitrary offset, triggered entirely from the host side.
  [`cbccc0dd6e`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/cbccc0dd6e)
- **`usb_hid` GET_REPORT no longer leaks the heap.** `reqlen` is the host's
  number; TinyUSB clamps the destination but nothing clamped the source, so an
  ordinary request with a 65 byte buffer sent 55 bytes past an 8 byte report —
  Python heap contents leaving the board over USB.
  [`40ede587be`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/40ede587be)

---

## 3. Display and graphics

### Defects

- **BMP files with a CORE header were decoded entirely wrong** — every field
  read at the Windows INFOHEADER offset, and a 3-byte palette read as 4-byte
  entries, which ran on into the pixel data.
  [`171c77f436`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/171c77f436)
- **1bpp dithering came out byte-reversed**: the writer packed 32 pixels MSB
  first, so a white run at x 0–7 appeared at x 24–31.
  [`2c91619f78`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/2c91619f78)
- **`lvfontio` ignored the font's real bit depth and advance**, decoding any
  font that was not the assumed depth incorrectly.
  [`645c24355b`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/645c24355b)
- **The terminal escape parser read past the end of its buffer** and could
  return more than it was given, which made the write loop spin forever with
  heap contents rendered onto the display on the way.
  [`c858e84d3f`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/c858e84d3f)
- **`gifio`'s writer buffer was sized without the 416-byte header** it writes
  first, so a narrow image overran it before a frame was encoded
  ([`464e59e974`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/464e59e974));
  a frame's length is now checked before its header is written
  ([`3a8f036e53`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/3a8f036e53));
  and `OnDiskGif(filename=...)` opens the file rather than a file literally
  named "filename"
  ([`5c8dd7a46c`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/5c8dd7a46c)).
- **TileGrid and display arguments were narrowed before they were validated**,
  so an init sequence longer than 65535 bytes initialised the display with a
  prefix of itself, and four TileGrid arguments reached `uint16_t` fields
  unchecked.
  [`1d42e8468a`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/1d42e8468a),
  [`1c85d7ad98`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/1c85d7ad98)
- **A freshly constructed `TilePaletteMapper` panicked the board** on subscript,
  dividing by a zero width.
  [`8f281be3c5`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8f281be3c5)
- **A vectorio shape in a group contributed garbage to the dirty area**, because
  the area lookup fell through with nothing written and still reported success.
  [`5b4d413bc7`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/5b4d413bc7)
- **A polygon above 32768 points truncated its stored length**, and above 65535
  the validation loop never terminated.
  [`47bb3093cb`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/47bb3093cb)
- **Filters never marked their destination bitmap dirty**, so a filter applied
  to a bitmap on screen showed nothing from the second frame on.
  [`fd1ac093d5`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/fd1ac093d5)
- **The palette bounds check was off by one**, so an index equal to the colour
  count read, and cached into, the allocation that follows the palette.
  [`d66c544860`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d66c544860)
- **The display init sequence was parsed with no bounds checking at all**; a
  short or malformed sequence sent whatever followed it to the bus.
  [`6a2deb8d5d`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/6a2deb8d5d)
- **A 46341 × 46341 grid overflowed its tile count in `int`**, allocated small
  and then filled the full count.
  [`51c585525b`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/51c585525b)
- **`bitmapfilter`**: the blend table clamped to the wrong end of its domain, so
  an additive blend rendered bright areas black
  ([`64a25e6032`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/64a25e6032)),
  and `morph`'s `offset` was read through the wrong union member, so only its low
  byte arrived and negative offsets were unreachable
  ([`d87b99e610`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d87b99e610)).
- **A transparent vectorio pixel was drawn anyway**, as colour 0, and its mask
  bit then hid every layer beneath it.
  [`a7ffd32edc`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/a7ffd32edc)
- **A tiled background refreshed only its first cell.** The shortcut for a
  one-tile bitmap also matched a grid showing that bitmap many times, and it
  overwrote the accumulated dirty area instead of merging with it.
  [`9117f0c9de`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/9117f0c9de)
- **The window cache this fork added went stale** when anything else programmed
  the bus — `bus.send()` carrying CASET, `bus.reset()`, or EPaperDisplay
  resetting the panel — which put a full-screen pixel stream into the wrong
  window.
  [`e372702035`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/e372702035)

### Speed (display path, not the interpreter)

The display path is measured and documented in `FORK-CHANGES.md` section 2; a
full-screen refresh on the Cardputer went from 12.7 ms to roughly half that, and
partial redraws improved more.

- The window is programmed once per refresh and the pixels continue into it,
  instead of four bus operations per subrectangle
  ([`a93a852a15`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/a93a852a15)),
  and a window that has not moved is not reprogrammed at all — 169 µs against
  5 µs for the comparison that replaces it
  ([`01e018b5ad`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/01e018b5ad)).
- The next subrectangle is composed while the previous one is on the bus
  ([`c7f9b26bf2`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/c7f9b26bf2)),
  with the rows spread evenly so the composition actually hides inside the
  transfer
  ([`fd371041a8`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/fd371041a8)),
  over a deeper SPI transaction queue
  ([`a7f1132659`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/a7f1132659)).
- Area buffers are sized to the area that fits in them rather than to the
  configured maximum
  ([`14f0134d43`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/14f0134d43)).
- A multi-tile fill no longer recomputes tile indices and unpacks through the
  generic bitmap accessor for every pixel
  ([`40965d56f8`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/40965d56f8)).
- An `OnDiskBitmap` is read a row at a time instead of a seek and a read per
  pixel
  ([`6226113155`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/6226113155)).
- `jpegio`'s early exits compared each side against itself and never fired, so
  every image was decoded in full however little was wanted
  ([`0951f17dcb`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/0951f17dcb)).
- `bitmaptools.boundary_fill` keeps its frontier by scanline instead of a Python
  list popped from the front, which was quadratic in the filled area
  ([`52cfd980a8`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/52cfd980a8)).
- Polygon edges use a conditional subtract instead of two modulos per edge per
  pixel
  ([`32298af148`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/32298af148)).
- The parallel display bus gets a usable DMA transfer size and queue depth
  ([`ae30e9b8db`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/ae30e9b8db)).
- Board tuning for the Cardputer: the ST7789 runs at its rated 80 MHz rather
  than 40 (37.4 → 71.6 Mbit/s measured)
  ([`9542f57da9`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/9542f57da9)),
  and the instruction cache is pinned at 16 kB with the measurement that says why
  ([`92487fba20`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/92487fba20)).

---

## 4. Audio

- **A looping MP3 went silent after one pass.** An input underflow at the end of
  the data is how a complete MP3 ends, but it was reported as a decode error, so
  the caller stopped instead of restarting.
  [`801c163c44`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/801c163c44)
  The I2S side had the matching half: a decoder is entitled to report DONE with
  an empty final buffer, and that ended playback instead of rewinding it.
  [`f95c170559`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/f95c170559)
- **A WAV file with a `JUNK` or `LIST` chunk before `fmt ` was rejected**, and
  the parser could read an uninitialised format. RIFF subchunks are walked now.
  [`2ba97d5c85`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/2ba97d5c85)
- **`Mixer` wrote one word past its mix buffer** for an odd sample count.
  [`b65c3fdd19`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/b65c3fdd19)
- **`RawSample` kept only a raw pointer into the caller's buffer**, so a sliced
  memoryview could be collected while playback was still reading it.
  [`f917e93040`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/f917e93040)
- **"Fill with silence" filled with full negative deflection.** `memset` repeats
  one byte, so 32768 truncated to 0 — silence for unsigned samples is 0x8000.
  Two places:
  [`7e132b6677`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/7e132b6677),
  [`a3130c81ff`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/a3130c81ff)
- **`PDMIn`'s `bit_depth` and `oversample` were narrowed before validation**, so
  zero passed the divisibility check and two values 256 apart were the same
  argument.
  [`afee33469a`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/afee33469a)
- **synthio**: the DDS accumulator could leave the waveform
  ([`2d821bb78d`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/2d821bb78d));
  the ring modulator sign-flipped at full scale, which is exactly what the
  default waveform produces
  ([`32dfb91ffc`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/32dfb91ffc));
  and a MIDI track that ends mid-message no longer parses past the end —
  `synthio.MidiTrack(b"\x00\x90")` was enough
  ([`10b83ce2c3`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/10b83ce2c3)).
- **The shared `sample_rate` setter validated nothing**, so zero could reach the
  DDS divisions and the I2S clock configuration.
  [`d8616e5158`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d8616e5158)
- **The mixer divided in software floating point twice per output word** — 44100
  times a second per voice, inside a callback with a deadline. It mixes in
  integers now.
  [`5099505f16`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/5099505f16)

---

## 5. Wi-Fi, BLE and networking

- **Scanning above channel 14.** `start_scanning_networks()` validated its
  arguments against 1–14 and the backend walked a hardcoded 2.4 GHz pattern, so
  a dual-band part could never report a 5 GHz network. The range is 1–165 and
  the pattern carries the 5 GHz channels; a channel the country setting refuses
  is skipped rather than ending the scan.
  [`d86a6544ef`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d86a6544ef)
- **A full-length PSK wrote its terminator past the password field**, into the
  next member of the config struct.
  [`d10b56229f`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d10b56229f)
- **An accepted socket started with an uninitialised timeout**, and a full send
  buffer is retried.
  [`a7ae8c2a30`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/a7ae8c2a30)
- **`getaddrinfo(proto=...)` raised "unexpected keyword argument"** because the
  fifth parameter was named `port` a second time — and `port=80` then matched
  both slots, so `proto` silently became 80 and travelled out in the returned
  tuples.
  [`a6e05cde9a`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/a6e05cde9a)
- **`wifi.radio.hostname` was passed as a pointer without its length**, so a
  bytearray or a memoryview slice set whatever followed it in memory, having
  bypassed validation.
  [`0d2dcaf8db`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/0d2dcaf8db)
- **BLE indications were thrown away.** The handler required
  `indication == 0`, and since NimBLE has already sent the confirmation the peer
  sees a healthy link while the data goes nowhere.
  [`8abe349e75`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8abe349e75)
- **`_bleio` allocated inside a critical section**, so a collection or a
  `MemoryError` could leave interrupts disabled with the nesting count stuck and
  the watchdog resetting the board.
  [`6126a31880`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/6126a31880)
- **An advertising structure was bounded only against the prefix**, so a device
  advertising `b"\x1f\x09"` made `memcmp` read 30 bytes past a two byte packet;
  the prefix side had the mirror problem and needed no radio at all.
  [`bfd9d99000`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/bfd9d99000)

---

## 6. Security, crypto and input validation

- **`getpass(prompt)` used the prompt as a format string**, so a `%` in it
  pulled words off the varargs area and dereferenced them.
  [`937f676376`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/937f676376)
- **`hashlib` discarded every PSA status.** The digest buffer is pre-filled with
  zeros, so a failed clone or finish returned a valid-looking all-zero digest —
  the worst outcome for a signature comparison.
  [`b35a96d7f9`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/b35a96d7f9)
- **`msgpack` multiplied an attacker-supplied element count by the element
  size** with no overflow check: seven bytes of input reached a `gc_alloc(0)`
  whose map still claimed half a billion slots
  ([`13e042f12c`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/13e042f12c)),
  and neither pack nor unpack checked the C stack, so nesting from the input
  smashed the stack the VM shares
  ([`5dbf8d2e6c`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/5dbf8d2e6c)).
- **`struct` took its repeat count through the wrong accessor**, so
  `calcsize("99999999999i")` returned a different number every call, and the
  count × item size product overflowed past both size checks.
  [`ea60ef1ed6`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/ea60ef1ed6)
- **A negative `start=` was not clamped**, so `spi.readinto(bytearray(10),
  start=-11)` wrote before the beginning of the buffer.
  [`8f1ae69b4c`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8f1ae69b4c)
- **`ipaddress` accepted anything.** A non-int, non-string, non-buffer argument
  built an object with no data whose first `print` took the board down
  ([`1d61a6c3fb`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/1d61a6c3fb)),
  and unchecked octets meant `"300.1.1.1"` parsed as 44.1.1.1 — a typo in a
  static address configured a different address instead of raising
  ([`ef9e0a08a1`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/ef9e0a08a1)).
- **`warnings` built its exception past the category**, so a Python subclass of
  `Warning` produced a native exception struct stamped with a Python class —
  four lines of Python took the board down.
  [`96db8127d5`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/96db8127d5)

---

## 7. Core Python and standard library

- **`del obj.attr` through the VM store shortcut left a null value behind**,
  which the next read handed to the interpreter: `(nil)` on unix, a reboot on the
  board.
  [`d81db5108d`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/d81db5108d)
- **A one-byte string is not proof of ASCII.** Indexing or iterating a non-ASCII
  string produced broken fragments, and interning one made a qstr that is not
  valid UTF-8.
  [`531d379b19`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/531d379b19)
- **A missing keyword-only argument named the wrong parameter** — a function
  missing `bravo` reported `__dir__`, because the name was read from a pointer
  into the bytecode as though it were an object array.
  [`1d2703fd76`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/1d2703fd76)
- **`os.chdir` committed the new path before the call that can fail**, leaving
  `getcwd()` reporting a directory that does not exist.
  [`e8541511e6`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/e8541511e6)
- **`random.randint(-2**31, 2**31-1)` needed a hard reset**: the range
  computation overflowed into a loop with no interrupt check
  ([`8bf46ef8d4`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8bf46ef8d4)),
  and `random.seed(0xEDA4BABA)` was the one seed in four billion that did not
  reproduce, because that value doubled as "not yet seeded"
  ([`8f75938e61`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/8f75938e61)).
- **`traceback.print_exception` could leave the caller's exception stripped** if
  printing raised — which a read-only CIRCUITPY under USB makes ordinary.
  [`52144ef896`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/52144ef896)
- **`rainbowio.colorwheel(-1)` returned −768** instead of a colour; a descending
  loop reaches that the moment it passes zero.
  [`79240386b5`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/79240386b5)
- **`time.localtime()` decomposed its own result twice** to recover the weekday
  and day of year, which `timeutils` exports directly.
  [`0aa78bfaf2`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/0aa78bfaf2)

---

## 8. Buses, pins and the espressif port

- **Subclassing `DigitalInOut` in Python rebooted the board.** All twenty
  bindings cast `self_in` straight to the native struct, which on a subclass is
  the instance object. Fixed across the module
  ([`9de837a98e`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/9de837a98e))
  and then in the last two entry points that had been missed
  ([`aad21a66ab`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/aad21a66ab));
  an invalid `drive_mode` now raises instead of silently meaning push-pull
  ([`07db34c647`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/07db34c647)).
- **A pin with no pull reported `PULL_UP`.** `get_pull()` read the pull-up bit
  and then ignored it, so `PULL_NONE` came back only when the HAL call failed.
  [`c265e45a20`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/c265e45a20)
- **Pin state is read from the register instead of through a HAL call** that
  fills a whole config struct — `get_value()` called `get_direction()` on every
  read.
  [`9507b11a5b`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/9507b11a5b)
- **PWM lost its duty cycle across a frequency change**, because only the
  resolution-scaled value was kept
  ([`cfb449b8c4`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/cfb449b8c4)),
  and the constructor narrowed `duty_cycle` before validating it, so 65536
  quietly became 0
  ([`70e3a8d942`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/70e3a8d942)).
- **A failed SPI reconfiguration longjmped out with the bus mutex held** and the
  handle pointing at freed memory; it returns a status now.
  [`e99f9e6508`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/e99f9e6508)
- **An unbalanced `enable_interrupts()` silently disabled every critical section
  in the port.** The only guard was an `assert` that compiles out.
  [`4611d0de72`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/4611d0de72)
- **`bitbangio` divided by zero on a zero frequency or baudrate** — a panic and
  a reboot rather than an exception
  ([`fe11e2d8b1`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/fe11e2d8b1));
  the ceiling added with that fix rejected rates that had worked and is gone
  ([`299d674439`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/299d674439));
  and a bus error read as an acknowledgement, so a failed I2C transfer was
  reported as complete
  ([`089c549133`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/089c549133)).
- **`adafruit_bus_device` faked the lock when an exception was pending**, so an
  ordinary auto-reload during a sensor read masked the real exception and
  released an outer `try_lock()`.
  [`f411d640f8`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/f411d640f8)
- **A short input buffer reported the wrong argument** in
  `writeto_then_readfrom`.
  [`0f41bb3d42`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/0f41bb3d42)
- **The Cardputer keyboard read one entry past both lookup tables** for key 56.
  [`cf437f0eae`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/cf437f0eae)
- **Board configuration**: channel state information enabled on the Cardputer
  ([`143666c60e`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/143666c60e)).

---

## 9. Tests and documentation

- Regression tests for the keyword-only and constructor paths, in the
  MicroPython suite's own style:
  [`c45e3c9d6a`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/c45e3c9d6a)
- `FORK-CHANGES.md`, the narrative record of the whole fork — what each change
  is for, what it measured, and what was tried and rejected:
  [`5322939030`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/5322939030),
  and the upstream merge of 2026-09-12 with its seventeen conflict decisions:
  [`0b4ab87849`](https://github.com/peterbay/circuitpython-esp-enhanced/commit/0b4ab87849)

---

## 10. What is deliberately not in this list

The interpreter performance work: VM dispatch tables, the qstr and class lookup
caches, the in-place frame builder, the allocator and the growth policies of
list, array, vstr and the readall buffer, 64-bit integer arithmetic, IRAM and
DRAM placement on the ESP32, and the follow-up fixes to each. Those are about
fifty commits between 2026-08-23 and 2026-09-11; `FORK-CHANGES.md` sections 1
and 5 describe them with the measurements that justify each one.

Also not listed: the native code emitter, which was attempted and removed
entirely (section 6 of `FORK-CHANGES.md` records why), and `usb_video`, which
bricks USB on this chip and stays off.
