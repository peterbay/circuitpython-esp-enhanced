"""Transmitter: send a numbered broadcast on channel 26, twice a second.

Drop this on one board as code.py and rx.py on another. Both print what they
see, so a plain serial reader on each is enough to watch the link.

The MAC header is built here on purpose. The module deals in whole frames and
leaves the header to Python, which is what makes it usable for a protocol that
does not exist yet.
"""

import time

import _bleio
import ieee802154
import wifi

# Wi-Fi, BLE and 802.15.4 share one 2.4 GHz front end. With either of the other
# two up, the coexistence arbiter cuts transmissions off mid-frame and every
# send() comes back as "rejected by coexistence".
wifi.radio.enabled = False
_bleio.adapter.enabled = False

# Channel 26 sits above the Wi-Fi band. The channels in the middle of it are
# usable but busy: with CCA on, most transmissions there are refused.
CHANNEL = 26
PAN = 0x1234
BROADCAST = 0xFFFF

# The short address comes from the chip's own MAC, so two boards running this
# same file cannot collide and neither has to be told which one it is.
ME = (wifi.radio.mac_address[4] << 8) | wifi.radio.mac_address[5]

radio = ieee802154.Radio(channel=CHANNEL, pan_id=PAN, short_address=ME)

print()
print("=== TX 0x%04X, kanal %d, PAN 0x%04X ===" % (ME, radio.channel, PAN))


def data_frame(seq, dst, payload):
    """A data frame with short addresses, no acknowledgement requested."""
    fcf = 0x0001            # frame type: data
    fcf |= 1 << 6           # PAN ID compression: source PAN is the destination's
    fcf |= 2 << 10          # destination addressing: short
    fcf |= 2 << 14          # source addressing: short
    header = bytes((
        fcf & 0xFF, fcf >> 8,
        seq & 0xFF,
        PAN & 0xFF, PAN >> 8,
        dst & 0xFF, dst >> 8,
        ME & 0xFF, ME >> 8,
    ))
    return header + payload


seq = 0
sent = 0
failed = 0
last_error = ""

while True:
    try:
        # cca=False: two nodes on a quiet channel do not need to defer, and
        # listening first is what refuses frames on a channel Wi-Fi is using.
        radio.send(data_frame(seq, BROADCAST, b"ahoj z %04X #%d" % (ME, seq)),
                   cca=False, timeout=0.3)
        sent += 1
    except Exception as e:  # noqa: BLE001 - counted rather than printed, so a
        # failing link does not turn into a flood on a port nobody is reading.
        failed += 1
        last_error = "%s: %s" % (type(e).__name__, e)

    seq += 1
    if sent % 20 == 0 or failed:
        print("odeslano %d, chyb %d%s" % (
            sent, failed, ", posledni: " + last_error if failed else ""))
    time.sleep(0.5)
