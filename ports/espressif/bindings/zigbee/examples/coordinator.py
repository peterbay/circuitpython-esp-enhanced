"""Form a Zigbee network and let devices join it.

Put this on one board and end_device.py on another. The coordinator forms the
network on first run and remembers it: the stack keeps the network in the chip's
non-volatile storage, so a later run resumes rather than forms. `factory_new`
says which of the two is about to happen, and factory_reset() forces the first.

Joining is only possible while the network is open. In BDB that is network
steering run on a device that is already on a network -- the same procedure that
joins an unjoined device opens the network of a joined one. A closed network
still answers a joining device's beacon request and then ignores it, which from
the other end looks like nothing happening at all.
"""

import time

import _bleio
import wifi
import zigbee

# Wi-Fi, BLE and 802.15.4 share one 2.4 GHz front end. Leaving the others up
# costs frames, and the loss is invisible -- the arbiter simply takes the radio.
wifi.radio.enabled = False
_bleio.adapter.enabled = False

# One channel rather than all sixteen: forming and joining both scan the mask,
# and naming one turns minutes into seconds. Pick one that is quiet -- the
# ieee802154 examples' survey.py finds out which.
CHANNEL = 20

# Set to True once to throw away the stored network and start over. A Trust
# Center remembers a device whose join it never saw finish, and ignores that
# device's later attempts, so a half-finished pairing can only be cleared here.
FORGET_NETWORK = False

stack = zigbee.Stack(role=zigbee.COORDINATOR, channels=1 << CHANNEL)
# Nothing can be asked of the stack before start(): the library keeps its state
# in statics that only then get filled in.
stack.start()

# Read straight after starting and before anything forms: forming a network is
# what stops it being factory new, so asking later always says "no".
was_factory_new = stack.factory_new
print()
print("=== KOORDINATOR %s, tovarni stav %s, distribuovana bezpecnost %s ===" % (
    "".join("%02X" % b for b in reversed(stack.extended_address)),
    was_factory_new, stack.distributed_security))

if FORGET_NETWORK and not was_factory_new:
    print("mazu ulozenou sit a restartuji")
    stack.factory_reset()   # does not return

# Far enough back that the first pass opens the network straight away.
# monotonic() counts from boot, so a plain 0.0 here means waiting out the whole
# interval before ever opening it -- during which a joining device finds a
# closed network and gives up.
was_joined = False
last_status = time.monotonic()
devices = {}

while True:
    event = stack.event()
    while event is not None:
        print("udalost %-34s status %3d akce %3d adresa 0x%04X" % (
            event["name"], event["status"], event["action"], event["address"]))
        # A device update carries the Trust Center's decision: 0 accepted,
        # 1 denied, 2 ignored. Ignored is what a half-finished earlier join
        # leaves behind.
        if event["name"].startswith("ZDO Device") and event["address"] != 0xFFFF:
            devices[event["address"]] = event["action"]
        event = stack.event()

    now = time.monotonic()

    if stack.joined and not was_joined:
        was_joined = True
        print("sit %s: kanal %d, PAN 0x%04X, ext.PAN %s" % (
            "zalozena" if was_factory_new else "obnovena",
            stack.channel, stack.pan_id,
            "".join("%02X" % b for b in reversed(stack.extended_pan_id))))

    # Nothing here drives commissioning. Forming, and opening the network
    # afterwards, are answered by the module from the stack's own signals --
    # calling steer() from here as well starts a second commissioning run on top
    # of the first, which is how a coordinator ends up with an open network that
    # never finishes admitting anyone. open_network() is the supported way to
    # extend the window once it has expired.

    if now - last_status >= 10.0:
        last_status = now
        print("-- adresa 0x%04X, kanal %d, PAN 0x%04X, zarizeni %s" % (
            stack.short_address, stack.channel, stack.pan_id,
            ", ".join("0x%04X akce %d" % kv for kv in devices.items()) or "zadna"))

    time.sleep(0.1)
