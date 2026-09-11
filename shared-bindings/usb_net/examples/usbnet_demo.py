"""A small web app on the board, reached over the USB network interface.

Needs ``usb_net`` enabled in boot.py and ``adafruit_httpserver`` in /lib.
From code.py::

    import usbnet_demo
    usbnet_demo.serve()

or, to keep a loop of your own::

    server = usbnet_demo.start()
    while True:
        server.poll()

The page shows what the board knows about itself and, on a board with a
NeoPixel, lets the browser set its colour.
"""

import gc
import json
import os
import time

import board
import microcontroller
import socketpool
import wifi
from adafruit_httpserver import GET, POST, JSONResponse, Request, Response, Server

try:
    import neopixel_write
    from digitalio import DigitalInOut, Direction

    _led_pin = DigitalInOut(board.NEOPIXEL)
    _led_pin.direction = Direction.OUTPUT
except (ImportError, AttributeError):
    _led_pin = None

_led = "#000000"

PAGE = """<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s</title>
<style>
  body { font: 16px/1.5 system-ui, sans-serif; margin: 2rem auto; max-width: 32rem; padding: 0 1rem; color: #222; }
  h1 { font-size: 1.4rem; }
  table { border-collapse: collapse; width: 100%%; }
  td { padding: .3rem .5rem; border-bottom: 1px solid #ddd; }
  td:first-child { color: #666; width: 40%%; }
  input[type=color] { width: 4rem; height: 2rem; vertical-align: middle; }
</style>
<h1>%s</h1>
<table>
  <tr><td>Board</td><td id="board"></td></tr>
  <tr><td>CircuitPython</td><td id="version"></td></tr>
  <tr><td>Uptime</td><td id="uptime"></td></tr>
  <tr><td>Free memory</td><td id="memory"></td></tr>
  <tr><td>CPU temperature</td><td id="temperature"></td></tr>
  <tr><td>Your address</td><td id="client"></td></tr>
  <tr id="ledrow" hidden><td>LED</td><td><input type="color" id="led"></td></tr>
</table>
<p id="error" style="color:#b00"></p>
<script>
const $ = id => document.getElementById(id);
async function refresh() {
  try {
    const s = await (await fetch("/api/status")).json();
    $("board").textContent = s.board;
    $("version").textContent = s.version;
    $("uptime").textContent = Math.floor(s.uptime) + " s";
    $("memory").textContent = s.memory_free + " B";
    $("temperature").textContent = s.temperature === null ? "n/a" : s.temperature.toFixed(1) + " \\u00b0C";
    $("client").textContent = s.client;
    if (s.led !== null) { $("ledrow").hidden = false; if (document.activeElement !== $("led")) $("led").value = s.led; }
    $("error").textContent = "";
  } catch (e) {
    $("error").textContent = "No answer from the board: " + e;
  }
}
$("led").addEventListener("input", async e => {
  await fetch("/api/led", { method: "POST", headers: { "Content-Type": "application/json" },
                            body: JSON.stringify({ color: e.target.value }) });
});
refresh();
setInterval(refresh, 2000);
</script>
</html>
"""


def _set_led(color):
    global _led
    if len(color) != 7 or color[0] != "#":
        raise ValueError("color must be #rrggbb")
    r, g, b = (int(color[i:i + 2], 16) for i in (1, 3, 5))
    if _led_pin is not None:
        # WS2812 takes green first.
        neopixel_write.neopixel_write(_led_pin, bytearray((g, r, b)))
    _led = color


def _status(request):
    try:
        temperature = microcontroller.cpu.temperature
    except (AttributeError, NotImplementedError):
        temperature = None
    return {
        "board": board.board_id,
        "version": os.uname().version,
        "uptime": time.monotonic(),
        "memory_free": gc.mem_free(),
        "temperature": temperature,
        "client": request.client_address[0],
        "led": _led if _led_pin is not None else None,
    }


def start(hostname="cardputer.home.arpa", port=80):
    """Create the server and start it; the caller polls it."""
    server = Server(socketpool.SocketPool(wifi.radio))
    page = PAGE % (hostname, hostname)

    @server.route("/", GET)
    def index(request: Request):
        return Response(request, page, content_type="text/html")

    @server.route("/api/status", GET)
    def status(request: Request):
        return JSONResponse(request, _status(request))

    @server.route("/api/led", POST)
    def led(request: Request):
        try:
            _set_led(request.json()["color"])
        except (ValueError, KeyError, TypeError) as e:
            return JSONResponse(request, {"error": str(e)}, status=(400, "Bad Request"))
        return JSONResponse(request, {"led": _led})

    # Every address the board has, which over USB is the one boot.py gave it.
    server.start("0.0.0.0", port)
    print("usbnet_demo: http://%s%s/" % (hostname, "" if port == 80 else ":%d" % port))
    return server


def serve(hostname="cardputer.home.arpa", port=80):
    """Start the server and serve until interrupted."""
    server = start(hostname, port)
    while True:
        server.poll()
