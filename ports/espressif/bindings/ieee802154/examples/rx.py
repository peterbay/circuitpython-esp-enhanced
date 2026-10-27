"""Receiver: print every frame addressed to this PAN on channel 26.

The counterpart to tx.py. Address filtering is left on, so this only sees frames
for PAN 0x1234 and for this node or the broadcast address -- which is the normal
case. For everything on the air regardless of who it is for, see monitor.py.
"""

import time

import _bleio
import ieee802154
import wifi

wifi.radio.enabled = False
_bleio.adapter.enabled = False

CHANNEL = 26
PAN = 0x1234

ME = (wifi.radio.mac_address[4] << 8) | wifi.radio.mac_address[5]

# queue=16: frames arriving while Python is busy printing wait here. What does
# not fit is counted in radio.lost, so a queue that is too small says so.
radio = ieee802154.Radio(channel=CHANNEL, pan_id=PAN, short_address=ME, queue=16)

print()
print("=== RX 0x%04X, kanal %d, PAN 0x%04X ===" % (ME, radio.channel, PAN))

heard = 0
last_status = time.monotonic()

while True:
    f = radio.receive()
    while f is not None:
        heard += 1
        d = f["data"]
        # Header as tx.py builds it: fcf(2) seq(1) dst pan(2) dst addr(2)
        # src addr(2), then the payload. The source PAN is not on the air
        # because the header sets PAN ID compression.
        seq = d[2]
        dst = d[5] | (d[6] << 8)
        src = d[7] | (d[8] << 8)
        print("#%-5d od 0x%04X pro 0x%04X seq=%-3d  %4d dBm lqi %2d  %s" % (
            heard, src, dst, seq, f["rssi"], f["lqi"], bytes(d[9:])))
        f = radio.receive()

    now = time.monotonic()
    if now - last_status >= 10.0:
        last_status = now
        # callbacks counts what the driver handed up, heard counts what came
        # back out of the queue. Equal means nothing was lost on the way.
        print("-- prijato %d, callbacku %d, ztraceno %d" % (
            heard, radio.callbacks, radio.lost))

    time.sleep(0.01)
