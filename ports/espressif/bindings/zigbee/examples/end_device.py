"""Join a Zigbee network as an end device.

The counterpart to coordinator.py. An end device is a leaf: it does not relay
for anyone, which is what lets a real one be battery powered.

Joining needs some coordinator or router on the same channel with its network
open. Without that this will scan, find nothing to join, and say so.

Getting an address is not the same as being let in. `joined` says an address
was assigned, which happens early; the Trust Center then decides whether to
hand over the network key, and a device that is refused holds its address for a
while and is then told to leave. That refusal is what a ZDO Leave event means
here.

`stack.authorized` is not the thing to watch on this side: the stack raises it
on the Trust Center, which is the coordinator. A device only ever learns that it
was accepted by not being asked to leave.
"""

import time

import _bleio
import wifi
import zigbee

wifi.radio.enabled = False
_bleio.adapter.enabled = False

# Must match the coordinator. Scanning all sixteen works but is slow enough to
# look like a failure.
CHANNEL = 20

stack = zigbee.Stack(role=zigbee.END_DEVICE, channels=1 << CHANNEL)
# Nothing can be asked of the stack before start(): the library keeps its state
# in statics that only then get filled in.
stack.start()
print()
print("=== KONCOVE ZARIZENI %s, tovarni stav %s, distribuovana bezpecnost %s ===" % (
    "".join("%02X" % b for b in reversed(stack.extended_address)),
    stack.factory_new, stack.distributed_security))

was_joined = False
attempts = 0
last_try = time.monotonic()
last_status = time.monotonic()

while True:
    event = stack.event()
    while event is not None:
        print("udalost %-34s status %3d akce %3d adresa 0x%04X" % (
            event["name"], event["status"], event["action"], event["address"]))
        event = stack.event()

    # Report every transition, not just the first. Latching on the first join
    # hides the fact that the device is joining over and over with a new
    # address each time.
    joined = stack.joined
    if joined != was_joined:
        was_joined = joined
        if joined:
            attempts += 1
            print("adresa prirazena: 0x%04X, kanal %d, PAN 0x%04X (pokus %d)" % (
                stack.short_address, stack.channel, stack.pan_id, attempts))
        else:
            print("adresa ztracena")

    now = time.monotonic()
    # Retry rather than give up: a coordinator whose network is closed leaves a
    # joining device with nothing to do, and steer() is how to ask again once it
    # has been opened.
    if not joined and now - last_try >= 15.0:
        last_try = now
        print("zkousim znovu najit sit")
        stack.steer()

    if now - last_status >= 10.0:
        last_status = now
        print("-- adresa 0x%04X, kanal %d, pokusu %d" % (
            stack.short_address, stack.channel, attempts))

    time.sleep(0.1)
