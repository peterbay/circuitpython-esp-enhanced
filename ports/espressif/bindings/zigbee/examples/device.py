"""Be any ZHA device type, and publish values a coordinator can read.

Change DEVICE_TYPE at the top and this becomes that kind of device: the
endpoint carries whatever clusters the type brings, and the coordinator sees
the same device id in the simple descriptor that is set here.

Pair with reader.py, which finds this device, lists what it is, and reads
everything it holds.

Attribute values are plain ZCL: temperature is hundredths of a degree Celsius,
humidity hundredths of a percent, level 0 to 254, on/off a bool. The units are
the specification's, not this module's, which is why 23.5 degrees is 2350.
"""

import math
import time

import _bleio
import wifi
import zigbee

wifi.radio.enabled = False
_bleio.adapter.enabled = False

# Any of the module's device type constants. Every one of them works here; the
# ones with values to publish are handled in publish() below, and the rest
# simply join and answer reads of their Basic cluster.
DEVICE_TYPE = zigbee.TEMPERATURE_SENSOR

# Must match the coordinator. Scanning all sixteen works but is slow enough to
# look like a failure.
CHANNEL = 20
ENDPOINT = 1

# A router is mains powered and relays for others; an end device is a leaf and
# may sleep. Controllers and switches are usually the latter.
ROLE = zigbee.END_DEVICE

endpoint = zigbee.Endpoint(ENDPOINT, DEVICE_TYPE)
stack = zigbee.Stack(role=ROLE, channels=1 << CHANNEL, endpoints=(endpoint,))
# Nothing can be asked of the stack or the endpoint before start(): the library
# keeps its state in statics that only then get filled in.
stack.start()

print()
print("=== ZARIZENI %s, typ 0x%04X, tovarni stav %s ===" % (
    "".join("%02X" % b for b in reversed(stack.extended_address)),
    DEVICE_TYPE, stack.factory_new))
print("clustery (server):", ", ".join("0x%04X" % c for c in endpoint.clusters()))
print("clustery (client):", ", ".join("0x%04X" % c for c in endpoint.clusters(zigbee.CLIENT)))


def publish(seconds):
    """Put a plausible value into whatever this device type measures.

    Writing an attribute is all a device does to publish: a coordinator that
    asked for reports is told, and one that reads gets the new value. Nothing
    here sends anything by hand.

    Only measurements. On/Off and Level are deliberately left alone: they are
    what something else sets, and a light that keeps overwriting its own state
    fights whoever is controlling it -- which looks exactly like commands being
    lost.
    """
    wave = math.sin(seconds / 10.0)
    written = []

    for cluster, attribute, value in (
        # Temperature, hundredths of a degree: 20.00 to 25.00 degC.
        (zigbee.TEMPERATURE_MEASUREMENT, 0x0000, int(2250 + 250 * wave)),
        # Humidity, hundredths of a percent.
        (zigbee.HUMIDITY_MEASUREMENT, 0x0000, int(5000 + 1000 * wave)),
        # Illuminance, the ZCL log scale.
        (zigbee.ILLUMINANCE_MEASUREMENT, 0x0000, int(30000 + 5000 * wave)),
        # Pressure, hPa.
        (zigbee.PRESSURE_MEASUREMENT, 0x0000, int(1013 + 10 * wave)),
        # Local temperature of a thermostat, hundredths of a degree.
        (zigbee.THERMOSTAT_CLUSTER, 0x0000, int(2250 + 250 * wave)),
    ):
        try:
            endpoint[cluster, attribute] = value
            written.append("0x%04X/0x%04X = %s" % (cluster, attribute, value))
        except ValueError:
            # This device type does not carry that cluster, which is the normal
            # case for all but one or two of them.
            pass
    return written


last_publish = 0.0
was_joined = False
started = time.monotonic()

while True:
    event = stack.event()
    while event is not None:
        print("udalost %-30s status %3d adresa 0x%04X" % (
            event["name"], event["status"], event["address"]))
        event = stack.event()

    if stack.joined and not was_joined:
        was_joined = True
        print("pripojeno: adresa 0x%04X, kanal %d, PAN 0x%04X" % (
            stack.short_address, stack.channel, stack.pan_id))

    # Another device writing an attribute here -- a coordinator turning a light
    # on, say -- arrives the same way a report does.
    written = stack.report()
    while written is not None:
        print("zapsano zvenci: cluster 0x%04X atribut 0x%04X = %s" % (
            written["cluster"], written["attribute"], written["value"]))
        written = stack.report()

    now = time.monotonic()
    if stack.joined and now - last_publish >= 5.0:
        last_publish = now
        values = publish(now - started)
        if values:
            print("publikovano:", "; ".join(values))

    time.sleep(0.2)
