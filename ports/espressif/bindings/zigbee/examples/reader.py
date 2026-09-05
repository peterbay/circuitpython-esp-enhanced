"""A coordinator that finds every device that joins and reads what it holds.

Forms a network, keeps it open, and for each device that announces itself:
asks what endpoints it has, asks each endpoint what it is, and then reads every
attribute of every cluster it recognises. Whatever the device type, this ends up
printing its values.

Pair with device.py, which can be any ZHA device type.

Nothing here polls the stack for commissioning; the module answers those signals
itself. This only asks questions of other devices.
"""

import time

import _bleio
import wifi
import zigbee

wifi.radio.enabled = False
_bleio.adapter.enabled = False

CHANNEL = 20
SOURCE_ENDPOINT = 1

# A gateway, not a Configuration Tool. The clusters an endpoint may read are
# exactly its client-role ones, and a Configuration Tool's client list holds
# only Basic -- so it could read the manufacturer and model of what joins, and
# nothing else. This reads whatever turns up, so it needs the type that is
# exempt from that rule.
gateway = zigbee.Endpoint(SOURCE_ENDPOINT, zigbee.CUSTOM_GATEWAY)
stack = zigbee.Stack(role=zigbee.COORDINATOR, channels=1 << CHANNEL, endpoints=(gateway,))
stack.start()

print()
print("=== KOORDINATOR %s, tovarni stav %s ===" % (
    "".join("%02X" % b for b in reversed(stack.extended_address)), stack.factory_new))

# Which attributes are worth asking for, per cluster. Reading an attribute a
# device does not have costs one status byte in the answer, so the lists are
# the useful ones rather than everything the specification allows.
INTERESTING = {
    zigbee.BASIC: (0x0000, 0x0004, 0x0005, 0x0007),
    zigbee.POWER_CONFIG: (0x0020, 0x0021),
    zigbee.ON_OFF: (0x0000,),
    zigbee.LEVEL: (0x0000,),
    zigbee.TEMPERATURE_MEASUREMENT: (0x0000, 0x0001, 0x0002),
    zigbee.HUMIDITY_MEASUREMENT: (0x0000,),
    zigbee.ILLUMINANCE_MEASUREMENT: (0x0000,),
    zigbee.PRESSURE_MEASUREMENT: (0x0000,),
    zigbee.FLOW_MEASUREMENT: (0x0000,),
    zigbee.OCCUPANCY_SENSING: (0x0000,),
    zigbee.CARBON_DIOXIDE_MEASUREMENT: (0x0000,),
    zigbee.PM2_5_MEASUREMENT: (0x0000,),
    zigbee.COLOR_CONTROL: (0x0000, 0x0001, 0x0007),
    zigbee.THERMOSTAT_CLUSTER: (0x0000, 0x0011, 0x0012),
    zigbee.WINDOW_COVERING_CLUSTER: (0x0008,),
    zigbee.DOOR_LOCK_CLUSTER: (0x0000,),
    zigbee.IAS_ZONE: (0x0000, 0x0002),
    zigbee.ELECTRICAL_MEASUREMENT: (0x0505, 0x0508, 0x050B),
    zigbee.METERING: (0x0000,),
}

# Attribute 0x0004 and 0x0005 of Basic are the manufacturer and the model, and
# both are strings; the rest of what is asked for here is numeric.
NAMES = {
    (zigbee.BASIC, 0x0000): "verze ZCL",
    (zigbee.BASIC, 0x0004): "vyrobce",
    (zigbee.BASIC, 0x0005): "model",
    (zigbee.BASIC, 0x0007): "napajeni",
    (zigbee.POWER_CONFIG, 0x0020): "napeti baterie",
    (zigbee.POWER_CONFIG, 0x0021): "baterie %",
    (zigbee.ON_OFF, 0x0000): "zapnuto",
    (zigbee.LEVEL, 0x0000): "uroven",
    (zigbee.TEMPERATURE_MEASUREMENT, 0x0000): "teplota",
    (zigbee.HUMIDITY_MEASUREMENT, 0x0000): "vlhkost",
    (zigbee.ILLUMINANCE_MEASUREMENT, 0x0000): "osvetleni",
    (zigbee.PRESSURE_MEASUREMENT, 0x0000): "tlak",
    (zigbee.THERMOSTAT_CLUSTER, 0x0000): "mistni teplota",
}

DEVICE_TYPES = {
    0x0000: "on/off switch", 0x0005: "configuration tool", 0x0009: "mains power outlet",
    0x000A: "door lock", 0x000B: "door lock controller", 0x0100: "on/off light",
    0x0101: "dimmable light", 0x0102: "color dimmable light", 0x0104: "dimmer switch",
    0x0105: "color dimmer switch", 0x0106: "light sensor", 0x0200: "shade",
    0x0201: "shade controller", 0x0202: "window covering", 0x0203: "window covering controller",
    0x0301: "thermostat", 0x0302: "temperature sensor",
}

# address -> {endpoint: (device_type, input_clusters)}
devices = {}
pending = []
last_poll = 0.0


def describe(cluster, attribute, value):
    name = NAMES.get((cluster, attribute))
    label = name if name else "0x%04X/0x%04X" % (cluster, attribute)
    # The scaled ones, so the printed value is the physical one.
    if cluster == zigbee.TEMPERATURE_MEASUREMENT and attribute == 0x0000:
        return "%s %.2f degC" % (label, value / 100.0)
    if cluster == zigbee.THERMOSTAT_CLUSTER and attribute == 0x0000:
        return "%s %.2f degC" % (label, value / 100.0)
    if cluster == zigbee.HUMIDITY_MEASUREMENT and attribute == 0x0000:
        return "%s %.2f %%" % (label, value / 100.0)
    if cluster == zigbee.POWER_CONFIG and attribute == 0x0021:
        return "%s %.1f %%" % (label, value / 2.0)
    return "%s %s" % (label, value)


while True:
    event = stack.event()
    while event is not None:
        print("udalost %-30s status %3d akce %3d adresa 0x%04X" % (
            event["name"], event["status"], event["action"], event["address"]))
        # A device announces itself once it is on the network for good, which is
        # the point at which it is worth asking what it is.
        if event["name"].startswith("ZDO Device Announce") and event["address"] != 0xFFFF:
            print("nove zarizeni 0x%04X, ptam se co je zac" % event["address"])
            devices.setdefault(event["address"], {})
            stack.discover(event["address"])
        event = stack.event()

    descriptor = stack.descriptor()
    while descriptor is not None:
        address = descriptor["address"]
        endpoint = descriptor["endpoint"]
        kind = DEVICE_TYPES.get(descriptor["device_type"], "neznamy typ")
        print("0x%04X endpoint %d: %s (0x%04X), profil 0x%04X" % (
            address, endpoint, kind, descriptor["device_type"], descriptor["profile"]))
        print("    vstupni clustery: %s" % ", ".join(
            "0x%04X" % c for c in descriptor["input_clusters"]))
        print("    vystupni clustery: %s" % ", ".join(
            "0x%04X" % c for c in descriptor["output_clusters"]))
        devices.setdefault(address, {})[endpoint] = descriptor["input_clusters"]
        # Read straight away, and then again on the poll below.
        for cluster in descriptor["input_clusters"]:
            if cluster in INTERESTING:
                pending.append((address, endpoint, cluster))
        descriptor = stack.descriptor()

    # One read at a time: a device that is asked for everything at once answers
    # some of it and drops the rest.
    if pending:
        address, endpoint, cluster = pending.pop(0)
        try:
            stack.read(address, endpoint, cluster, INTERESTING[cluster],
                       source_endpoint=SOURCE_ENDPOINT)
        except RuntimeError as error:
            print("cteni 0x%04X ep %d cluster 0x%04X selhalo: %s" % (
                address, endpoint, cluster, error))

    value = stack.report()
    while value is not None:
        origin = "hlaseni" if value["kind"] == 1 else "odpoved"
        if value["status"] != 0:
            print("0x%04X ep %d: 0x%04X/0x%04X nedostupny (status %d)" % (
                value["address"], value["endpoint"], value["cluster"],
                value["attribute"], value["status"]))
        else:
            print("0x%04X ep %d %s: %s  (rssi %d)" % (
                value["address"], value["endpoint"], origin,
                describe(value["cluster"], value["attribute"], value["value"]),
                value["rssi"]))
        value = stack.report()

    now = time.monotonic()
    if now - last_poll >= 15.0:
        last_poll = now
        if not devices:
            print("-- adresa 0x%04X, kanal %d, PAN 0x%04X, zadne zarizeni" % (
                stack.short_address, stack.channel, stack.pan_id))
        for address, endpoints in devices.items():
            for endpoint, clusters in endpoints.items():
                for cluster in clusters:
                    if cluster in INTERESTING:
                        pending.append((address, endpoint, cluster))

    time.sleep(0.2)
