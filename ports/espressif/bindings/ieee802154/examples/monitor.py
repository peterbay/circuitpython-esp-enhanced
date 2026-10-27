"""An 802.15.4 monitor: capture every frame on a channel, decode it, and guess
which protocol is running on top -- Zigbee, Thread/6LoWPAN, or neither.

Promiscuous mode turns off address filtering, so frames for any PAN and any node
are delivered. It also turns off automatic acknowledgement, which is what a
monitor wants: acknowledging traffic that is not addressed to it would corrupt
the exchange it is watching. The radio still checks the checksum and drops frames
that fail it, so a monitor cannot see corrupted frames -- that is a hardware
limit, not something this module hides.

readinto() rather than receive(): a monitor is exactly the case where allocating
a dict and a bytes object per frame starts costing frames.
"""

import time

import _bleio
import ieee802154
import wifi

# One 2.4 GHz front end for all three radios. Leaving Wi-Fi or BLE up costs
# frames, and the loss is invisible -- the arbiter simply takes the radio away.
wifi.radio.enabled = False
_bleio.adapter.enabled = False

CHANNEL = 26
# Set to a tuple of channels to hop. Every frame sent while the radio is on a
# different channel is missed, so a hopping monitor sees a sample, not a record.
# Zigbee and Thread both live in 11-26; survey.py finds the busy ones first.
HOP = ()
HOP_SECONDS = 2.0

FRAME_TYPE = ("beacon", "data", "ack", "cmd", "multipurpose", "fragment", "extended", "?")

# MAC commands, by the identifier that follows the header. A device being paired
# is easy to spot here: it sweeps the band sending beacon requests, then
# association requests to whoever answered.
MAC_CMD = {
    0x01: "association request",
    0x02: "association response",
    0x03: "disassociation",
    0x04: "data request",
    0x05: "PAN ID conflict",
    0x06: "orphan notification",
    0x07: "beacon request",
    0x08: "coordinator realignment",
    0x09: "GTS request",
}

radio = ieee802154.Radio(channel=CHANNEL, queue=32)
radio.promiscuous = True

frame = bytearray(125)
meta = bytearray(16)


def addr(data, at, mode):
    """Return one address and how many bytes it took."""
    if mode == 2:
        return "%04X" % (data[at] | (data[at + 1] << 8)), 2
    if mode == 3:
        return "".join("%02X" % b for b in reversed(data[at:at + 8])), 8
    return "-", 0


def classify(data, at, length, secured, ftype):
    """Guess the protocol above the MAC from the start of the payload.

    Heuristics, not proof: everything here reads a handful of bytes that happen
    to be distinctive, and any payload can imitate them by accident. Treat a
    single frame as a hint and a stream of consistent answers as an identification.
    """
    n = length - at
    if n <= 0:
        return "-"

    b = data[at]

    if ftype == 3:
        return "MAC: " + MAC_CMD.get(b, "cmd 0x%02X" % b)

    if ftype == 0:
        # A beacon says who it is outright, and says whether the network will
        # take new devices -- which is the difference between a sensor that
        # joins and one that sends beacon requests forever.
        if at + 1 >= length:
            return "beacon"
        superframe = data[at] | (data[at + 1] << 8)
        who = "koord" if superframe & 0x4000 else "router"
        joining = "PRIPOJOVANI POVOLENO" if superframe & 0x8000 else "zavreno"

        # The payload follows the superframe specification (2), the GTS
        # specification (1, no descriptors when the count is zero) and the
        # pending address specification (1).
        p = at + 2
        if p < length:
            gts = data[p]
            p += 1 + (3 * (gts & 0x07) + 1 if gts & 0x07 else 0)
        if p < length:
            pend = data[p]
            p += 1 + 2 * (pend & 0x07) + 8 * ((pend >> 4) & 0x07)
        if p >= length:
            return "beacon %s, %s" % (who, joining)

        pid = data[p]
        if pid == 0x00:
            # Zigbee beacon payload: protocol id, stack profile and network
            # version packed in one byte, capacity byte, then the extended PAN
            # ID that identifies the network across a channel change.
            extra = ""
            if p + 11 < length:
                profile = data[p + 1] & 0x0F
                epid = "".join("%02X" % b for b in reversed(data[p + 4:p + 12]))
                extra = " profil %d ext.PAN %s" % (profile, epid)
            return "Zigbee beacon %s, %s%s" % (who, joining, extra)
        if pid == 0x03:
            return "Thread beacon %s, %s" % (who, joining)
        return "beacon protokol 0x%02X, %s" % (pid, joining)

    # Zigbee network layer: frame control, low two bits the frame type, then a
    # four-bit protocol version that is 2 for Zigbee PRO and 1 for Zigbee 2004.
    nwk_type = b & 0x03
    nwk_ver = (b >> 2) & 0x0F
    if nwk_type in (0, 1, 3) and nwk_ver in (1, 2):
        return "Zigbee NWK v%d %s" % (
            nwk_ver, ("data", "cmd", "?", "inter-PAN")[nwk_type])

    # 6LoWPAN dispatch, which is what Thread carries. Thread also secures at the
    # link layer, so an unsecured frame matching this is far more likely to be
    # something else that happens to start with the same bits.
    lowpan = None
    if 0x60 <= b <= 0x7F:
        lowpan = "IPHC"
    elif 0x80 <= b <= 0xBF:
        lowpan = "mesh"
    elif 0xC0 <= b <= 0xC7:
        lowpan = "frag1"
    elif 0xE0 <= b <= 0xE7:
        lowpan = "fragN"
    elif b == 0x41:
        lowpan = "IPv6"
    if lowpan:
        return "%s 6LoWPAN %s" % ("Thread?" if secured else "mozna", lowpan)

    return "?"


def decode(data, length):
    """Split a MAC header into (description, protocol guess, payload offset)."""
    if length < 3:
        return "kratky ramec", "-", length
    fcf = data[0] | (data[1] << 8)
    ftype = fcf & 0x07
    dst_mode = (fcf >> 10) & 0x03
    src_mode = (fcf >> 14) & 0x03
    version = (fcf >> 12) & 0x03
    secured = bool(fcf & 0x08)
    flags = "".join((
        "S" if secured else ".",         # security enabled
        "P" if fcf & 0x10 else ".",      # frame pending
        "A" if fcf & 0x20 else ".",      # acknowledgement requested
        "C" if fcf & 0x40 else ".",      # PAN ID compression
    ))

    at = 3
    dst_pan = src_pan = "-"
    if dst_mode:
        dst_pan = "%04X" % (data[at] | (data[at + 1] << 8))
        at += 2
    dst, used = addr(data, at, dst_mode)
    at += used
    # With PAN ID compression the source PAN is not on the air; it is the
    # destination's. Without it, a source address carries its own PAN.
    if src_mode and not (fcf & 0x40):
        src_pan = "%04X" % (data[at] | (data[at + 1] << 8))
        at += 2
    elif src_mode:
        src_pan = dst_pan
    src, used = addr(data, at, src_mode)
    at += used

    # With link-layer security on, an auxiliary header sits between the MAC
    # header and the payload, so the payload does not start here and cannot be
    # read anyway. Its length depends on the key identifier mode.
    if secured and at < length:
        control = data[at]
        key_mode = (control >> 3) & 0x03
        at += 1 + 4 + (0, 0, 5, 9)[key_mode]

    header = "%-6s v%d %s seq=%3d  %s/%s -> %s/%s" % (
        FRAME_TYPE[ftype], version, flags, data[2],
        src_pan, src, dst_pan, dst)
    return header, classify(data, at, length, secured, ftype), at


print()
print("=== MONITOR kanal %d, promisk %s ===" % (radio.channel, radio.promiscuous))

seen = 0
protocols = {}
last_status = time.monotonic()
last_hop = time.monotonic()
hop_at = 0

while True:
    n = radio.readinto(frame, meta)
    if n is not None:
        seen += 1
        rssi = meta[1] - 256 if meta[1] > 127 else meta[1]
        header, proto, at = decode(frame, n)
        protocols[proto] = protocols.get(proto, 0) + 1
        payload = bytes(frame[at:n])
        print("#%-5d ch%-3d %4d dBm lqi %3d  %s | %-18s [%d B] %s" % (
            seen, meta[3], rssi, meta[2], header, proto, len(payload), payload))
        continue

    now = time.monotonic()
    if HOP and now - last_hop >= HOP_SECONDS:
        last_hop = now
        hop_at = (hop_at + 1) % len(HOP)
        radio.channel = HOP[hop_at]

    if now - last_status >= 10.0:
        last_status = now
        # lost counts frames the radio delivered that the queue had no room for;
        # callbacks counts everything the driver handed up. Their difference is
        # the only place a monitor can silently miss traffic it did receive.
        print("-- kanal %d: zachyceno %d, callbacku %d, ztraceno %d | %s" % (
            radio.channel, seen, radio.callbacks, radio.lost,
            ", ".join("%s x%d" % kv for kv in protocols.items()) or "nic"))

    time.sleep(0.002)
