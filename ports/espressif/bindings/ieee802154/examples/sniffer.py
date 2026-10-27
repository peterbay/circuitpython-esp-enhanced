"""Capture a channel and take each frame apart layer by layer.

monitor.py says what a frame is. This says what is in it: the MAC header, then
whatever sits on top -- Zigbee's network, application-support and cluster
layers, or the 802.15.4 security header that is as far as Thread lets anyone
read.

What can be read at all depends on what is encrypted, and on a working network
most of it is:

* MAC header -- always readable. Addresses, PAN, sequence number.
* MAC security header -- readable when present. Level, frame counter, key index.
  The MAC payload after it is encrypted. Thread secures here, so a Thread
  network gives up its addressing and nothing else.
* Zigbee NWK header -- readable. Zigbee does not usually secure at the MAC
  layer, so the network header, both short addresses, the radius and the
  security header with its frame counter are all in the clear.
* Zigbee APS and ZCL -- encrypted with the network key on an established
  network. Readable during joining, in beacons, on inter-PAN frames, and from
  Green Power devices.

So a sniffer without keys sees who is talking to whom, how often, and roughly
what about. It does not see attribute values on a running network.

Set KEY to the network key and it decrypts the rest: AES-CCM* is built here out
of aesio's ECB, so nothing outside this file is needed. The coordinator will
tell you its own through `zigbee.Stack.network_key`.

Decryption is verified, not attempted: a frame whose tag does not match is
reported as such and never half-decoded. That is what tells two networks on one
channel apart -- measured with a Zigbee network of our own and a Sonoff plug
still attached to an older one, on channel 20 at the same time, the frames of
the first decoded and the second were reported as belonging to another key.
"""

import time

import _bleio
import aesio
import ieee802154
import wifi

# One 2.4 GHz front end for all three radios. Leaving Wi-Fi or BLE up costs
# frames, and the loss is invisible.
wifi.radio.enabled = False
_bleio.adapter.enabled = False

CHANNEL = 20

# The network key, if you have it, as sixteen bytes. Without it everything
# above the Zigbee network layer stays encrypted. The coordinator will tell you
# its own:
#
#     >>> stack.network_key
#
# and it goes here as, for instance,
# KEY = bytes([0x2f, 0x8d, ...])
KEY = None

# Print the raw bytes under every decode.
SHOW_HEX = True

FRAME_TYPE = ("beacon", "data", "ack", "cmd", "multipurpose", "fragment", "extended", "?")

MAC_CMD = {
    0x01: "association request", 0x02: "association response", 0x03: "disassociation",
    0x04: "data request", 0x05: "PAN ID conflict", 0x06: "orphan notification",
    0x07: "beacon request", 0x08: "coordinator realignment", 0x09: "GTS request",
}

SECURITY_LEVEL = ("none", "MIC-32", "MIC-64", "MIC-128",
                  "ENC", "ENC-MIC-32", "ENC-MIC-64", "ENC-MIC-128")

NWK_CMD = {
    0x01: "route request", 0x02: "route reply", 0x03: "network status",
    0x04: "leave", 0x05: "route record", 0x06: "rejoin request",
    0x07: "rejoin response", 0x08: "link status", 0x09: "network report",
    0x0A: "network update", 0x0B: "end device timeout request",
    0x0C: "end device timeout response", 0x0D: "link power delta",
}

APS_CMD = {
    0x05: "transport key", 0x06: "update device", 0x07: "remove device",
    0x08: "request key", 0x09: "switch key", 0x0E: "tunnel",
    0x0F: "verify key", 0x10: "confirm key",
}

ZDO_CMD = {
    0x0000: "NWK addr req", 0x0001: "IEEE addr req", 0x0002: "node desc req",
    0x0003: "power desc req", 0x0004: "simple desc req", 0x0005: "active EP req",
    0x0006: "match desc req", 0x0013: "device annce", 0x0021: "bind req",
    0x0022: "unbind req", 0x0031: "mgmt LQI req", 0x0034: "mgmt leave req",
    0x0036: "mgmt permit joining req", 0x8000: "NWK addr rsp",
    0x8001: "IEEE addr rsp", 0x8002: "node desc rsp", 0x8004: "simple desc rsp",
    0x8005: "active EP rsp", 0x8006: "match desc rsp", 0x8021: "bind rsp",
    0x8031: "mgmt LQI rsp", 0x8034: "mgmt leave rsp", 0x8036: "mgmt permit rsp",
}

ZCL_GLOBAL = {
    0x00: "read attributes", 0x01: "read attributes response",
    0x02: "write attributes", 0x04: "write attributes response",
    0x06: "configure reporting", 0x07: "configure reporting response",
    0x0A: "report attributes", 0x0B: "default response",
    0x0C: "discover attributes", 0x0D: "discover attributes response",
}

CLUSTER = {
    0x0000: "Basic", 0x0001: "Power", 0x0003: "Identify", 0x0004: "Groups",
    0x0005: "Scenes", 0x0006: "OnOff", 0x0008: "Level", 0x000A: "Time",
    0x0019: "OTA", 0x0020: "Poll", 0x0021: "GreenPower", 0x0102: "WindowCovering",
    0x0201: "Thermostat", 0x0300: "ColorControl", 0x0400: "Illuminance",
    0x0402: "Temperature", 0x0403: "Pressure", 0x0405: "Humidity",
    0x0406: "Occupancy", 0x0500: "IASZone", 0x0702: "Metering",
    0x0B04: "ElectricalMeasurement", 0x1000: "Touchlink",
}


# ---------------------------------------------------------------------------
# AES-CCM*, which is what Zigbee secures frames with. aesio has ECB, and CCM* is
# CTR for the payload plus CBC-MAC for the tag, both built from ECB -- so the
# whole of it fits here rather than needing anything from the stack.
# ---------------------------------------------------------------------------

def _xor_into(target, source):
    for i in range(len(target)):
        target[i] ^= source[i]


def ccm_star_decrypt(key, nonce, ciphertext, aad, mic_length=4):
    """Decrypt and verify. Returns the plaintext, or None if the tag is wrong.

    A wrong tag means the key is wrong, or the frame was not for this network,
    or it was damaged -- so this never hands back bytes it could not
    authenticate.
    """
    cipher = aesio.AES(key, aesio.MODE_ECB)
    block = bytearray(16)
    keystream = bytearray(16)

    def encrypt_block(source):
        cipher.encrypt_into(bytes(source), keystream)
        return bytes(keystream)

    # Counter blocks: flags, the 13-byte nonce, then a two-byte counter.
    def counter_block(index):
        out = bytearray(16)
        out[0] = 1                      # L - 1, with L = 2
        out[1:14] = nonce
        out[14] = (index >> 8) & 0xFF
        out[15] = index & 0xFF
        return out

    # The payload, decrypted with the keystream from counter 1 upwards.
    plain = bytearray(len(ciphertext))
    for offset in range(0, len(ciphertext), 16):
        stream = encrypt_block(counter_block(offset // 16 + 1))
        chunk = ciphertext[offset:offset + 16]
        for i in range(len(chunk)):
            plain[offset + i] = chunk[i] ^ stream[i]

    # The tag, over the authenticated data and then the plaintext.
    state = bytearray(16)
    first = bytearray(16)
    first[0] = (0x40 if aad else 0) | (((mic_length - 2) // 2) << 3) | 1
    first[1:14] = nonce
    first[14] = (len(plain) >> 8) & 0xFF
    first[15] = len(plain) & 0xFF
    _xor_into(state, first)
    state[:] = encrypt_block(state)

    if aad:
        # The length goes in front, two bytes for anything short enough.
        header = bytearray(2)
        header[0] = (len(aad) >> 8) & 0xFF
        header[1] = len(aad) & 0xFF
        stream = bytes(header) + bytes(aad)
        for offset in range(0, len(stream), 16):
            chunk = stream[offset:offset + 16]
            block[:] = bytes(chunk) + bytes(16 - len(chunk))
            _xor_into(state, block)
            state[:] = encrypt_block(state)

    for offset in range(0, len(plain), 16):
        chunk = plain[offset:offset + 16]
        block[:] = bytes(chunk) + bytes(16 - len(chunk))
        _xor_into(state, block)
        state[:] = encrypt_block(state)

    # The tag is masked with the keystream of counter zero.
    mask = encrypt_block(counter_block(0))
    expected = bytes(state[i] ^ mask[i] for i in range(mic_length))
    return bytes(plain), expected


def u16(data, at):
    return data[at] | (data[at + 1] << 8)


def u32(data, at):
    return data[at] | (data[at + 1] << 8) | (data[at + 2] << 16) | (data[at + 3] << 24)


def eui(data, at):
    return "".join("%02X" % b for b in reversed(data[at:at + 8]))


def name_cluster(cluster):
    return CLUSTER.get(cluster, "0x%04X" % cluster)


def mac_header(data, length, out):
    """Decode the 802.15.4 header. Returns where the payload starts, or None."""
    if length < 3:
        return None
    fcf = u16(data, 0)
    ftype = fcf & 0x07
    security = bool(fcf & 0x08)
    pending = bool(fcf & 0x10)
    ack = bool(fcf & 0x20)
    intra_pan = bool(fcf & 0x40)
    dst_mode = (fcf >> 10) & 0x03
    src_mode = (fcf >> 14) & 0x03

    at = 3   # frame control and sequence number
    line = ["%s seq %d" % (FRAME_TYPE[ftype], data[2])]

    if dst_mode:
        line.append("dstPAN %04X" % u16(data, at))
        at += 2
        if dst_mode == 2:
            line.append("dst %04X" % u16(data, at))
            at += 2
        else:
            line.append("dst %s" % eui(data, at))
            at += 8
    if src_mode:
        if not intra_pan:
            line.append("srcPAN %04X" % u16(data, at))
            at += 2
        if src_mode == 2:
            line.append("src %04X" % u16(data, at))
            at += 2
        else:
            line.append("src %s" % eui(data, at))
            at += 8

    if ack:
        line.append("ackreq")
    if pending:
        line.append("pending")
    out.append("MAC   " + " ".join(line))

    if security:
        # The auxiliary security header. Thread lives behind this one.
        if at >= length:
            return None
        control = data[at]
        level = control & 0x07
        key_mode = (control >> 3) & 0x03
        at += 1
        counter = u32(data, at)
        at += 4
        note = "level %s, key mode %d, counter %d" % (
            SECURITY_LEVEL[level], key_mode, counter)
        if key_mode == 2:
            at += 4 + 1
        elif key_mode == 3:
            at += 8 + 1
        elif key_mode == 1:
            at += 1
        out.append("MACSEC " + note + " -- payload encrypted here")
        return None if level >= 4 else at
    return at


def zcl(data, at, length, cluster, out):
    if at >= length:
        return
    control = data[at]
    kind = control & 0x03
    manuf = bool(control & 0x04)
    to_client = bool(control & 0x08)
    at += 1
    manuf_code = None
    if manuf:
        manuf_code = u16(data, at)
        at += 2
    if at + 1 >= length:
        return
    seq = data[at]
    command = data[at + 1]
    at += 2

    if kind == 0:
        what = ZCL_GLOBAL.get(command, "global 0x%02X" % command)
    else:
        what = "%s cmd 0x%02X" % (name_cluster(cluster), command)
    line = "ZCL   %s, seq %d, %s" % (what, seq, "to client" if to_client else "to server")
    if manuf_code is not None:
        line += ", manufacturer 0x%04X" % manuf_code
    out.append(line)

    # The two that carry values worth naming.
    if kind == 0 and command in (0x01, 0x0A):
        p = at
        while p + 2 < length:
            attr = u16(data, p)
            p += 2
            if command == 0x01:
                status = data[p]
                p += 1
                if status != 0:
                    out.append("        attr 0x%04X status %d" % (attr, status))
                    continue
            if p >= length:
                break
            atype = data[p]
            p += 1
            size = {0x10: 1, 0x18: 1, 0x20: 1, 0x21: 2, 0x22: 3, 0x23: 4,
                    0x28: 1, 0x29: 2, 0x2B: 4, 0x30: 1, 0x31: 2,
                    0x39: 4, 0xE2: 4, 0xF1: 16}.get(atype)
            if size is None:
                if atype in (0x41, 0x42) and p < length:
                    size = 1 + data[p]
                else:
                    out.append("        attr 0x%04X type 0x%02X (delka neznama)" % (attr, atype))
                    break
            raw = data[p:p + size]
            p += size
            if atype in (0x20, 0x30, 0x10, 0x18, 0x28):
                value = raw[0] if raw else 0
            elif atype in (0x21, 0x31, 0x29):
                value = u16(raw, 0) if len(raw) >= 2 else 0
                if atype == 0x29 and value > 0x7FFF:
                    value -= 0x10000
            elif atype in (0x23, 0x2B, 0x39, 0xE2):
                value = u32(raw, 0) if len(raw) >= 4 else 0
            elif atype in (0x41, 0x42):
                value = bytes(raw[1:])
            else:
                value = bytes(raw)
            out.append("        attr 0x%04X = %s" % (attr, value))
    elif kind == 0 and command == 0x0B and at + 1 < length:
        out.append("        odpoved na cmd 0x%02X, status %d" % (data[at], data[at + 1]))


def aps(data, at, length, out):
    if at >= length:
        return
    control = data[at]
    kind = control & 0x03
    delivery = (control >> 2) & 0x03
    secured = bool(control & 0x20)
    at += 1

    if kind == 1:
        # An APS command. These are the joining conversation, and the ones that
        # matter are readable even when the payload after them is not.
        if secured:
            out.append("APS   command, encrypted")
            return
        if at < length:
            out.append("APS   command: %s" % APS_CMD.get(data[at], "0x%02X" % data[at]))
        return
    if kind == 2:
        out.append("APS   ack")
        return

    group = None
    dst_ep = None
    if delivery == 3:
        group = u16(data, at)
        at += 2
    else:
        dst_ep = data[at]
        at += 1
    if at + 4 > length:
        return
    cluster = u16(data, at)
    profile = u16(data, at + 2)
    at += 4
    src_ep = data[at] if at < length else 0
    at += 1
    counter = data[at] if at < length else 0
    at += 1

    where = "group 0x%04X" % group if group is not None else "ep %d" % dst_ep
    out.append("APS   %s <- ep %d, profil 0x%04X, cluster %s, counter %d%s" % (
        where, src_ep, profile, name_cluster(cluster), counter,
        ", encrypted" if secured else ""))
    if secured:
        return

    if profile == 0x0000:
        # ZDO: the cluster id is the command, and the payload starts with a
        # transaction sequence number.
        out.append("ZDO   %s" % ZDO_CMD.get(cluster, "0x%04X" % cluster))
        if cluster == 0x0013 and at + 10 < length:
            out.append("        annce: adresa %04X, %s" % (
                u16(data, at + 1), eui(data, at + 3)))
    else:
        zcl(data, at, length, cluster, out)


def nwk(data, at, length, out):
    if at + 8 > length:
        return
    nwk_start = at
    fcf = u16(data, at)
    kind = fcf & 0x03
    version = (fcf >> 2) & 0x0F
    multicast = bool(fcf & 0x0100)
    secured = bool(fcf & 0x0200)
    source_route = bool(fcf & 0x0400)
    dst_ieee = bool(fcf & 0x0800)
    src_ieee = bool(fcf & 0x1000)
    at += 2

    dst = u16(data, at)
    src = u16(data, at + 2)
    radius = data[at + 4]
    seq = data[at + 5]
    at += 6

    line = "NWK   v%d %s %04X -> %04X, radius %d, seq %d" % (
        version, ("data", "cmd", "?", "inter-PAN")[kind], src, dst, radius, seq)
    if dst_ieee and at + 8 <= length:
        line += ", dstIEEE %s" % eui(data, at)
        at += 8
    if src_ieee and at + 8 <= length:
        line += ", srcIEEE %s" % eui(data, at)
        at += 8
    if multicast and at < length:
        at += 1
    if source_route and at < length:
        relays = data[at]
        line += ", pres %d uzlu" % relays
        at += 2 + 2 * relays
    out.append(line)

    if secured:
        if at >= length:
            return
        secure_start = at
        control = data[at]
        level = control & 0x07
        key_mode = (control >> 3) & 0x03
        extended = bool(control & 0x20)
        at += 1
        counter = u32(data, at)
        at += 4
        note = "counter %d, key mode %d" % (counter, key_mode)
        if extended and at + 8 <= length:
            note += ", zdroj %s" % eui(data, at)
            at += 8
        if key_mode == 1 and at < length:
            note += ", key seq %d" % data[at]
            at += 1
        body = length - at - 4
        if KEY is None or body <= 0 or not extended:
            out.append("NWKSEC %s -- payload sifrovany (%d B + MIC)" % (note, max(0, body)))
            return

        # The nonce is the source address, the frame counter and the security
        # control byte -- with the level put back to what Zigbee actually uses.
        # The level travels as zero on the air, and using that would compute the
        # wrong tag.
        real_control = (control & 0xF8) | 0x05
        nonce = bytearray(13)
        nonce[0:8] = data[secure_start + 5:secure_start + 13]
        nonce[8] = counter & 0xFF
        nonce[9] = (counter >> 8) & 0xFF
        nonce[10] = (counter >> 16) & 0xFF
        nonce[11] = (counter >> 24) & 0xFF
        nonce[12] = real_control

        # Everything from the network header through the security header is
        # authenticated, with the same corrected level byte.
        aad = bytearray(data[nwk_start:at])
        aad[secure_start - nwk_start] = real_control

        mic = bytes(data[at + body:at + body + 4])
        plain, expected = ccm_star_decrypt(
            KEY, bytes(nonce), bytes(data[at:at + body]), bytes(aad))
        if expected != mic:
            out.append("NWKSEC %s -- desifrovani selhalo (jiny klic nebo jina sit)" % note)
            return
        out.append("NWKSEC %s -- desifrovano" % note)

        # Decoded from a copy, because the layers above expect one buffer with
        # absolute offsets and the plaintext is shorter than what it replaces.
        rebuilt = bytearray(data[:at]) + plain
        if kind == 1:
            out.append("NWK   command: %s" % NWK_CMD.get(
                rebuilt[at], "0x%02X" % rebuilt[at]))
        else:
            aps(rebuilt, at, len(rebuilt), out)
        return

    if kind == 1:
        if at < length:
            out.append("NWK   command: %s" % NWK_CMD.get(data[at], "0x%02X" % data[at]))
        return
    aps(data, at, length, out)


def beacon(data, at, length, out):
    if at + 1 >= length:
        return
    superframe = u16(data, at)
    who = "koordinator" if superframe & 0x4000 else "router"
    joining = "OTEVRENO" if superframe & 0x8000 else "zavreno"
    p = at + 2
    if p < length:
        gts = data[p]
        p += 1 + (3 * (gts & 0x07) + 1 if gts & 0x07 else 0)
    if p < length:
        pend = data[p]
        p += 1 + 2 * (pend & 0x07) + 8 * ((pend >> 4) & 0x07)
    line = "BEACON %s, pripojovani %s" % (who, joining)
    if p < length:
        pid = data[p]
        if pid == 0x00 and p + 11 < length:
            line += ", Zigbee profil %d, ext.PAN %s, hloubka %d" % (
                data[p + 1] & 0x0F, eui(data, p + 4), (data[p + 2] >> 3) & 0x0F)
        elif pid == 0x03:
            line += ", Thread"
        else:
            line += ", protokol 0x%02X" % pid
    out.append(line)


def lowpan(data, at, length, out):
    b = data[at]
    if 0x60 <= b <= 0x7F:
        what = "IPHC, komprimovana IPv6 hlavicka"
    elif 0x80 <= b <= 0xBF:
        what = "mesh header"
    elif 0xC0 <= b <= 0xC7:
        what = "fragment 1 z %d B" % (((b & 0x07) << 8) | data[at + 1] if at + 1 < length else 0)
    elif 0xE0 <= b <= 0xE7:
        what = "dalsi fragment"
    elif b == 0x41:
        what = "nekomprimovana IPv6"
    else:
        return False
    out.append("6LoWPAN %s" % what)
    return True


def decode(data, length):
    out = []
    fcf = u16(data, 0)
    ftype = fcf & 0x07
    at = mac_header(data, length, out)
    if at is None or at >= length:
        return out

    if ftype == 0:
        beacon(data, at, length, out)
    elif ftype == 3:
        out.append("MAC   command: %s" % MAC_CMD.get(data[at], "0x%02X" % data[at]))
    elif ftype == 1:
        # Zigbee's network layer and 6LoWPAN both start here, and they are told
        # apart by what the first byte can legally be.
        first = data[at]
        version = (first >> 2) & 0x0F
        if (first & 0x03) in (0, 1, 3) and version in (1, 2):
            nwk(data, at, length, out)
        elif not lowpan(data, at, length, out):
            out.append("data   neznamy protokol, prvni bajt 0x%02X" % first)
    return out


radio = ieee802154.Radio(channel=CHANNEL, queue=32)
radio.promiscuous = True

frame = bytearray(125)
meta = bytearray(16)
start = time.monotonic()
count = 0

print()
print("=== SNIFFER KANAL %d ===" % CHANNEL)
print("Ctrl-C konci. Sifrovane vrstvy jsou oznacene.")

while True:
    n = radio.readinto(frame, meta)
    if n is None:
        time.sleep(0.001)
        continue
    count += 1
    rssi = meta[1] - 256 if meta[1] > 127 else meta[1]
    print("%8.3f  #%d  %d B  rssi %d  lqi %d" % (
        time.monotonic() - start, count, n, rssi, meta[2]))
    for line in decode(frame, n):
        print("    " + line)
    if SHOW_HEX:
        print("    hex " + "".join("%02x" % b for b in frame[:n]))
