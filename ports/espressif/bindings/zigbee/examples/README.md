# Zigbee examples

All four run on a Seeed XIAO ESP32-C5 with this fork's firmware. Two boards are
enough: one coordinator, one device.

| Script | What it is |
| --- | --- |
| `coordinator.py` | The smallest coordinator: forms a network, opens it, prints what joins. |
| `end_device.py` | The smallest device: joins, and says whether it got in. |
| `reader.py` | A coordinator that discovers every device that joins and reads what it holds. |
| `device.py` | A device of any ZHA type, publishing values for `reader.py` to read. |
| `outlet.py` | A coordinator that finds a switchable outlet and turns it on and off ten times. |

Set `CHANNEL` to the same value in both halves of a pair. Scanning all sixteen
channels works but is slow enough to look like a failure.

## Being a device

`device.py` becomes whichever device type is named at the top:

```python
DEVICE_TYPE = zigbee.TEMPERATURE_SENSOR
```

Every ZHA type the library can build is available as a module constant:
`ON_OFF_SWITCH`, `CONFIGURATION_TOOL`, `MAINS_POWER_OUTLET`, `DOOR_LOCK`,
`DOOR_LOCK_CONTROLLER`, `ON_OFF_LIGHT`, `DIMMABLE_LIGHT`,
`COLOR_DIMMABLE_LIGHT`, `DIMMER_SWITCH`, `COLOR_DIMMER_SWITCH`, `LIGHT_SENSOR`,
`SHADE`, `SHADE_CONTROLLER`, `WINDOW_COVERING`, `WINDOW_COVERING_CONTROLLER`,
`THERMOSTAT`, `TEMPERATURE_SENSOR`, `CUSTOM_GATEWAY`.

The clusters an endpoint carries follow from its type; `endpoint.clusters()`
lists them. Values are written straight to the endpoint:

```python
sensor = zigbee.Endpoint(1, zigbee.TEMPERATURE_SENSOR)
stack = zigbee.Stack(role=zigbee.END_DEVICE, channels=1 << 20, endpoints=(sensor,))
stack.start()
sensor[zigbee.TEMPERATURE_MEASUREMENT, 0x0000] = 2350   # 23.50 degC
```

Units are the ZCL's: temperature and humidity in hundredths, level 0 to 254,
on/off a bool. Writing an attribute is the whole of publishing -- anything that
asked to be told is told.

## Reading other devices

A read is originated from the **client** side of a cluster: the set of clusters
an endpoint may read turns out to be exactly its client-role cluster list. An
endpoint that carries the right client cluster reads it without any special
treatment -- a gateway is not a requirement, only the general answer when the
clusters are not known in advance, which is `reader.py`'s case:

```python
gateway = zigbee.Endpoint(1, zigbee.CUSTOM_GATEWAY)
stack = zigbee.Stack(role=zigbee.COORDINATOR, channels=1 << 20, endpoints=(gateway,))
```

Reading from an endpoint whose client list does not have the cluster fails with
`ezb_zcl_read_attr_cmd_req failed (5)`. The 5 is `EZB_ZCL_ERR_NOT_FOUND`, which
says a resource was missing but not which one; that it is the client cluster is
what the measurements below show, not something the error code states. The call
raises before anything is queued for sending.

Measured on two boards, six endpoints:

| Source endpoint | client clusters | reads accepted |
| --- | --- | --- |
| `CONFIGURATION_TOOL` | 0x0000 | 0x0000 |
| `TEMPERATURE_SENSOR` | 0x0003 | 0x0003 |
| `DIMMER_SWITCH` | 0x0003, 0x0006, 0x0008 | 0x0003, 0x0006, 0x0008 |
| `COLOR_DIMMER_SWITCH` | 0x0003, 0x0006, 0x0008, 0x0300 | 0x0003, 0x0006, 0x0008, 0x0300 |
| `ON_OFF_LIGHT` | none | none |
| `CUSTOM_GATEWAY` | none | everything |

Every row but the last matches its client list exactly. `ON_OFF_LIGHT` cannot
read even Basic, which it carries as a *server*: holding a cluster as a server is
about answering, not asking. `CUSTOM_GATEWAY` is the exception -- it carries
fewer clusters than `CONFIGURATION_TOOL` and reads all of them, because it is
built by a different constructor which marks the endpoint as a gateway.

So a controller that already knows what it talks to does not need a gateway: a
`DIMMER_SWITCH` endpoint reads On/Off and Level from a light quite happily.
`CUSTOM_GATEWAY` is for the case where the clusters are not known in advance.

`Endpoint.clusters(zigbee.CLIENT)` answers the question directly for any
endpoint.

Discovery and reads are both asked for and answered later:

```python
stack.discover(address)          # -> stack.descriptor()
stack.read(address, endpoint, cluster, (0x0000,))   # -> stack.report()
```

`descriptor()` gives the endpoint's device type and cluster lists; `report()`
gives attribute values and command answers, told apart by `kind`.

Neither blocks, and neither confirms anything. A call that returns has been
accepted for sending -- not transmitted, and certainly not delivered. `report()`
returning None means the queue is empty at that moment: an answer may still be
in flight, may never come because the device is asleep or out of range, or may
have arrived and been dropped, which `lost_reports` counts.

Writing an attribute on another device is the same shape, with the ZCL type
given so the value is laid out for it:

```python
stack.write(address, endpoint, zigbee.IDENTIFY, 0x0000, 0x21, 30)
```

The answer arrives as a record of kind 4, and it is worth reading rather than
assuming. Measured against a device on the network:

| what was written | id in the answer | status |
| --- | --- | --- |
| Identify time, which is settable | 0xFFFF | 0 |
| On/Off state, read-only from the network | 0x0000 | 136 (0x88, read only) |
| an attribute that does not exist | 0x4321 | 134 (0x86, unsupported attribute) |

An id of 0xFFFF means every attribute in the request was written; otherwise the
id names one that was not. Most measured values are read-only from the network,
so this is for settings rather than for readings -- a sensor publishes by
writing its own attribute, not by being written to.

To be told about a value instead of polling for it:

```python
stack.configure_report(address, endpoint, zigbee.TEMPERATURE_MEASUREMENT,
                       0x0000, 0x29, minimum=10, maximum=60, change=50)
```

`0x29` is the ZCL type of the attribute, which the device checks.

## Controlling other devices

`Stack.command` sends any cluster command, with the command id and payload the
specification defines. Turning an outlet on is cluster 0x0006, command 0x01:

```python
stack.command(address, endpoint, zigbee.ON_OFF, 0x01)   # on
stack.command(address, endpoint, zigbee.ON_OFF, 0x00)   # off
stack.command(address, endpoint, zigbee.LEVEL, 0x04, bytes([128, 10, 0]))
```

`Stack.command` goes through the library's generic command request rather than
its per-cluster ones. On that path, and in this version, no local cluster check
was seen: all eight clusters tried were accepted from every endpoint, including
the `ON_OFF_LIGHT` that could not read a single one. The per-cluster command
functions are documented as checking, so this is a property of the path the
module uses, not of ZCL commands in general.

What a command is refused for is decided at the far end, and that answer does
come back -- as a `report()` record of kind 3, carrying the command id in
`attribute` and the ZCL status. Measured against a device on the network:

| what was sent | status |
| --- | --- |
| On/Off command 0x01, which it supports | 0 |
| On/Off command 0x42, which does not exist | 128 (0x80, unsupported command) |
| command 0x01 on cluster 0x0402, which it does not carry | 195 (0xC3, unsupported cluster) |

Only with `response=True`, which is the default; `response=False` asks the far
end not to answer, and then there is nothing. Note that in one run the first
command sent immediately after discovery produced no answer at all, so a single
missing record is not proof of anything.

## Finding devices without waiting for them to join

A device announces itself when it joins, and only then, so a coordinator that
was restarted hears nothing. `Stack.neighbors()` gives the stack's own view
instead:

```python
for entry in stack.neighbors():
    print(hex(entry["address"]), entry["lqi"], entry["rssi"])
```

That table is built from traffic rather than stored: after a reset it starts
empty and fills over the next few seconds, as routers send their link status
every fifteen seconds and end devices poll their parent. `outlet.py` waits for
it rather than reading it once. Only end devices are children -- a router that
joined is recorded with `relationship` 2, a sibling, and is just as reachable.

What does persist across a reset is the network and the security state of the
devices on it, which is why nothing has to pair again. It does not survive
reflashing: the firmware image is written from offset 0 and reaches over the
`nvs` partition, so every flash creates a new network and everything has to
join again.

## What was measured

Both pairs were run on two XIAO ESP32-C5 boards. A `TEMPERATURE_SENSOR` built by
`device.py` was discovered by `reader.py` as device type 0x0302 on profile
0x0104 with input clusters 0x0402, 0x0003 and 0x0000, and its readings came back
as real values. A Sonoff mains plug joined the same coordinator and was
described as device type 0x0009 with clusters 0x0000, 0x0003, 0x0004, 0x0005,
0x0006 and the manufacturer's 0xFC57, plus a Touchlink endpoint on 13 and a
Green Power one on 242.

Command answers were measured separately, against a `MAINS_POWER_OUTLET` built
from `device.py`: a supported command answered with status 0 and the state read
back matched, an unsupported command id answered 0x80, and a command on a
cluster the device does not carry answered 0xC3.

`outlet.py` was then run against that same Sonoff plug: it found it in the
neighbour table at 0x1D03 with IEEE address 00124B002B4879E9 and RSSI -56, saw
its On/Off cluster on endpoint 1, and switched it on and off ten times with five
seconds between. Each state was read back from the plug afterwards and matched
what it had been told.

`reader.py` keeps no store of its own across a restart. It does not need one for
devices that are still on the network -- `neighbors()` finds those -- but the
endpoint and cluster lists it collected are gone, and it discovers them again.
