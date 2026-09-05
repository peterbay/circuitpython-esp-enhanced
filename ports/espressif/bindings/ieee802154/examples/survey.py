"""Walk all sixteen channels and report what is on each one.

Finding a Zigbee or Thread network means finding its channel first, and neither
announces itself unasked: in a non-beacon network, beacons are only sent in reply
to a beacon request, and a monitor has no business transmitting one. So this
listens instead, and reports two independent things per channel -- how many
frames arrived, and how much energy was there.

Energy without frames usually means Wi-Fi: channels 11-22 sit underneath it, and
a busy access point reads as a strong carrier that never decodes.
"""

import time

import _bleio
import ieee802154
import wifi

wifi.radio.enabled = False
_bleio.adapter.enabled = False

# Seconds per channel. Zigbee end devices can be quiet for minutes at a time, so
# a short survey finds routers and coordinators, not the whole network.
DWELL = 6.0

radio = ieee802154.Radio(channel=11, queue=32)
radio.promiscuous = True

frame = bytearray(125)
meta = bytearray(16)

print()
print("=== PRUZKUM kanalu, %.0f s na kanal ===" % DWELL)
print("kanal  ramcu  nejsilnejsi  sum   PAN")

for channel in range(11, 27):
    radio.channel = channel
    # Drop anything the previous channel left in the queue, or it lands on this
    # channel's tally.
    while radio.readinto(frame, meta) is not None:
        pass

    frames = 0
    best = -128
    pans = []
    end = time.monotonic() + DWELL
    while time.monotonic() < end:
        n = radio.readinto(frame, meta)
        if n is None:
            time.sleep(0.002)
            continue
        frames += 1
        rssi = meta[1] - 256 if meta[1] > 127 else meta[1]
        if rssi > best:
            best = rssi
        # The destination PAN is the first field after the sequence number,
        # present whenever the frame carries a destination address at all.
        fcf = frame[0] | (frame[1] << 8)
        if n >= 5 and (fcf >> 10) & 0x03:
            pan = "%04X" % (frame[3] | (frame[4] << 8))
            if pan not in pans:
                pans.append(pan)

    # Measured after the dwell, because energy_detect takes the radio out of
    # receive: anything sent while it is measuring is missed.
    noise = radio.energy_detect(0.01)

    print("%5d  %5d  %11s  %4d  %s" % (
        channel, frames, "%d dBm" % best if frames else "-", noise,
        " ".join(pans) if pans else ""))

print("hotovo")
