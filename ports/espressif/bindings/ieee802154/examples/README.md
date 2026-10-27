# `ieee802154` examples

Each file is standalone: copy one onto a board as `code.py` and read its serial
output. They need no libraries and no `settings.toml`.

| File | What it does |
| --- | --- |
| `tx.py` | Sends a numbered broadcast twice a second on channel 26, PAN 0x1234. |
| `rx.py` | Prints frames addressed to that PAN, with source, sequence, RSSI and LQI. |
| `monitor.py` | Promiscuous capture: every frame on the channel, MAC header decoded, with a guess at the protocol above it. |
| `survey.py` | Walks all sixteen channels and reports frames, signal and noise on each. |

`tx.py` on one board and `rx.py` on another is the smallest working link.
`monitor.py` replaces either of them to watch instead of taking part, and
`survey.py` finds the channel worth watching in the first place.

Each board takes its short address from its own MAC, so two boards running the
same file cannot collide and neither has to be told which one it is.

## Things that are easy to get wrong

**Turn Wi-Fi and BLE off.** All three radios share one 2.4 GHz front end. With
either of the others up, the coexistence arbiter cuts transmissions off mid-frame
and every `send()` fails with *rejected by coexistence*. Every example starts by
disabling them.

**Channel 26.** Channels in the middle of the band work but sit under Wi-Fi, and
with CCA on most transmissions there are refused as *channel busy*. Use
`energy_detect()` to find a quiet one rather than guessing.

**A frame is the MAC header plus the payload, nothing else.** The length byte and
the checksum belong to the radio, which adds them on transmit and takes them off
on receive. If a receiver needs to know how long something is, put that in the
payload.

**A monitor cannot see corrupted frames.** The radio verifies the checksum and
drops what fails, before any of this can look at it. Promiscuous mode switches
off address filtering, not the checksum.

**A monitor must not acknowledge.** Setting `promiscuous = True` turns
`auto_ack` off for exactly that reason: acknowledging traffic addressed to
someone else corrupts the exchange being watched.

## Watching a Zigbee or Thread network

Both run on plain 802.15.4, so `monitor.py` sees their frames and guesses which
is which from the first bytes after the MAC header. Zigbee's network layer
carries a protocol version of 2 there; Thread carries a 6LoWPAN dispatch byte
and, unlike Zigbee, secures at the link layer -- so the `S` flag in the MAC
header is the cleanest single discriminator. Beacons say it outright, through a
protocol ID of `0x00` for Zigbee and `0x03` for Thread.

These are heuristics over a handful of bytes and any payload can imitate them.
The per-protocol tally in the status line is the thing to read: a stream of
consistent answers identifies a network, one frame does not.

A device being paired is the easiest thing to catch, because it is the only time
it is loud: it sweeps the whole band sending beacon requests, so parking on one
channel and waiting will see it. What comes back is a beacon, and the beacon says
whether the network will accept it -- `PRIPOJOVANI POVOLENO` against `zavreno` is
bit 15 of the superframe specification. A network that answers every beacon
request with `zavreno` is not broken; joining is simply not enabled on its
coordinator.

Once joined, a battery sensor is nearly silent: it reports on an event and then
checks in every so often. Finding it again means triggering it deliberately.

## Reading what went missing

`radio.callbacks` counts frames the driver handed up; `radio.lost` counts those
the queue had no room for. If `callbacks` matches what the script printed and
`lost` is zero, nothing was dropped between the radio and Python — which leaves
the air itself as the only place a frame can have gone.

## sniffer.py

`monitor.py` says what a frame is; `sniffer.py` says what is in it. It walks the
layers: the 802.15.4 header, the auxiliary security header when there is one,
then Zigbee's network, application-support and cluster layers, or a 6LoWPAN
dispatch byte.

How far it gets depends on what is encrypted, which is worth knowing before
reaching for it:

| layer | readable without keys |
| --- | --- |
| MAC header | always -- addresses, PAN, sequence |
| MAC security header | yes; the payload after it is not. Thread secures here |
| Zigbee NWK header | yes -- both addresses, radius, frame counter, source IEEE |
| Zigbee APS and ZCL | no, on an established network |

So without keys it shows who talks to whom and how often, and during joining --
when the APS commands are still in the clear -- rather more than that.

With the network key in `KEY` it decrypts the network layer and carries on into
APS and ZCL. AES-CCM* is implemented in the script itself from `aesio`'s ECB
mode, so there is nothing else to install. The coordinator hands over its own
key:

```python
>>> stack.network_key
```

Anyone holding that key can read and forge traffic on the network, so it wants
the same care as the network itself.

Decryption is verified rather than assumed: a frame whose tag does not match is
reported as belonging to another key and never half-decoded. Measured on channel
20 with two Zigbee networks in the air at once, frames of the network whose key
was loaded decoded to `NWK command: link status` and the other network's were
reported as not ours.

What it does not do: APS-layer encryption, which the joining conversation uses
on top of the network layer and which needs the link key; and Thread, whose
payload is behind MAC-layer security.
