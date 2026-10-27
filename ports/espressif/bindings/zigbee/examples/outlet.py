"""Find a switchable outlet on the network and turn it on and off ten times.

Works with any device that carries the On/Off cluster -- a Sonoff plug, or
device.py set to MAINS_POWER_OUTLET. The outlet has to have joined already;
this does not wait for one to pair.

The devices are taken from the stack's own neighbour table. That table is built
from traffic, not stored: after a reset it starts empty and fills over the next
few seconds, so this waits before reading it. The network and the devices'
security state do persist, which is why nothing has to pair again.
"""

import time

import _bleio
import wifi
import zigbee

wifi.radio.enabled = False
_bleio.adapter.enabled = False

CHANNEL = 20
SOURCE_ENDPOINT = 1

# On/Off cluster commands, from the specification.
OFF = 0x00
ON = 0x01
TOGGLE = 0x02

CYCLES = 10
HOLD = 5.0

# A gateway endpoint, for reading the state back rather than for sending: the
# clusters an endpoint may read are exactly its client-role ones, and a gateway
# is exempt from that. A DIMMER_SWITCH endpoint would do just as well here,
# since its client list has On/Off -- the gateway is for not having to know that
# in advance.
gateway = zigbee.Endpoint(SOURCE_ENDPOINT, zigbee.CUSTOM_GATEWAY)
stack = zigbee.Stack(role=zigbee.COORDINATOR, channels=1 << CHANNEL, endpoints=(gateway,))
stack.start()

print()
print("=== KOORDINATOR %s ===" % "".join(
    "%02X" % b for b in reversed(stack.extended_address)))

# start() returns as soon as the stack is running; forming or resuming the
# network happens after that, and until it has there is no table to read.
deadline = time.monotonic() + 20.0
while not stack.joined and time.monotonic() < deadline:
    time.sleep(0.2)
if not stack.joined:
    raise RuntimeError("sit se nezalozila")
print("sit: kanal %d, PAN 0x%04X" % (stack.channel, stack.pan_id))

# Who the stack knows. The table fills from traffic after a reset rather than
# being restored, so this waits for it rather than reading it once: a router
# announces itself in its link status, which it sends every fifteen seconds, and
# an end device when it next polls its parent.
print("cekam, az se ozvou sousede")
entries = ()
deadline = time.monotonic() + 40.0
while time.monotonic() < deadline:
    entries = stack.neighbors()
    if entries:
        break
    time.sleep(1.0)

# Not filtered to relationship 1: only end devices are children. A router that
# joined is recorded as a sibling, and it is just as reachable. Everything but
# this device's own parent is worth asking, and a coordinator has no parent.
KINDS = ("koordinator", "router", "koncove zarizeni")
RELATIONS = ("rodic", "dite", "sourozenec", "zadny")
known = []
for entry in entries:
    if entry["address"] in (0x0000, 0xFFFF):
        continue
    known.append(entry["address"])
    print("zna zarizeni 0x%04X, %s, %s, LQI %d, RSSI %d, %s" % (
        entry["address"],
        KINDS[entry["device_type"]] if entry["device_type"] < 3 else "?",
        RELATIONS[entry["relationship"]] if entry["relationship"] < 4 else "?",
        entry["lqi"], entry["rssi"],
        "".join("%02X" % b for b in reversed(entry["extended_address"]))))

if not known:
    print("zadne zname zarizeni; pripoj zasuvku a spust znovu")

# Which of them has an On/Off cluster, and on which endpoint.
outlets = []
for address in known:
    stack.discover(address)

deadline = time.monotonic() + 5.0
while time.monotonic() < deadline:
    descriptor = stack.descriptor()
    while descriptor is not None:
        if zigbee.ON_OFF in descriptor["input_clusters"]:
            outlets.append((descriptor["address"], descriptor["endpoint"]))
            print("spinatelne: 0x%04X endpoint %d, typ 0x%04X" % (
                descriptor["address"], descriptor["endpoint"], descriptor["device_type"]))
        descriptor = stack.descriptor()
    time.sleep(0.1)

if not outlets:
    print("nic s clusterem On/Off (0x%04X) se nenaslo" % zigbee.ON_OFF)

for cycle in range(CYCLES):
    for command, label in ((ON, "zapnuto"), (OFF, "vypnuto")):
        for address, endpoint in outlets:
            stack.command(address, endpoint, zigbee.ON_OFF, command,
                          source_endpoint=SOURCE_ENDPOINT)
        print("cyklus %d/%d: %s" % (cycle + 1, CYCLES, label))

        # Read the state back, so what is printed is what the outlet says rather
        # than what it was told.
        held = time.monotonic() + HOLD
        asked = False
        while time.monotonic() < held:
            if not asked and time.monotonic() > held - HOLD + 1.0:
                asked = True
                for address, endpoint in outlets:
                    stack.read(address, endpoint, zigbee.ON_OFF, 0x0000,
                               source_endpoint=SOURCE_ENDPOINT)
            value = stack.report()
            while value is not None:
                if value["cluster"] == zigbee.ON_OFF and value["status"] == 0:
                    print("    0x%04X hlasi: %s" % (
                        value["address"], "zapnuto" if value["value"] else "vypnuto"))
                value = stack.report()
            time.sleep(0.1)

print("hotovo")
