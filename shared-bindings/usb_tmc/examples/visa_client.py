"""Talk to the usbtmc_demo instrument from a PC with pyvisa.

    pip install pyvisa pyvisa-py pyusb libusb-package
    python visa_client.py [resource]

Linux: pyusb needs access to the device (a udev rule, or run as root).
Windows: there is no USBTMC driver in the system; bind WinUSB to the
"CircuitPython USBTMC" interface with Zadig once. pyvisa-py finds the
instrument through libusb-1.0 from libusb-package; a stray libusb0 on the
machine would be picked first and see nothing, so that DLL goes first on the
path below.
"""

import os
import sys
import time

if sys.platform == "win32":
    import libusb_package

    os.environ["PATH"] = os.path.dirname(libusb_package.find_library("libusb-1.0")) + os.pathsep + os.environ["PATH"]

import pyvisa  # noqa: E402

VID, PID = 0x303A, 0x81DA  # Espressif, the Cardputer's CircuitPython PID

rm = pyvisa.ResourceManager("@py")
if len(sys.argv) > 1:
    resource = sys.argv[1]
else:
    found = [r for r in rm.list_resources("USB?*INSTR") if "::%d::%d::" % (VID, PID) in r]
    if not found:
        raise SystemExit("no instrument found; USB resources: %s" % (rm.list_resources("USB?*"),))
    resource = found[0]
print("instrument:", resource)

inst = rm.open_resource(resource)
inst.timeout = 2000
print("*IDN? ->", inst.query("*IDN?").strip())

inst.write("*RST")
for i in range(5):
    print("temperature %s C, uptime %s s" % (inst.query("MEAS:TEMP?").strip(), inst.query("MEAS:TIME?").strip()))
    time.sleep(0.5)

for state in ("ON", "OFF"):
    inst.write("OUTP " + state)
    print("OUTP %s -> OUTP? %s" % (state, inst.query("OUTP?").strip()))

inst.write("NO:SUCH:COMMAND")
print("after an unknown command, SYST:ERR? ->", inst.query("SYST:ERR?").strip())
print("SYST:ERR? again ->", inst.query("SYST:ERR?").strip())

# pyvisa-py has no read_stb() for USB; the class request itself is short.
dev = inst.visalib.sessions[inst.session].interface.usb_dev
itf = int(resource.split("::")[4]) if resource.count("::") >= 5 else 0
inst.write("*ESE 255")
inst.write("NO:SUCH:COMMAND")
# A write returns as soon as the message is delivered; *OPC? waits until the
# instrument has processed everything before it, so the status is current.
inst.query("*OPC?")
status = bytes(dev.ctrl_transfer(0xA1, 128, 2, itf, 3, timeout=1000))
print("status byte 0x%02x (0x20 = event summary, 0x04 = error queued)" % status[2])
print("*ESR? ->", inst.query("*ESR?").strip(), " SYST:ERR? ->", inst.query("SYST:ERR?").strip())

n = 50
t0 = time.time()
for i in range(n):
    inst.query("MEAS:TIME?")
print("%d queries in %.2f s" % (n, time.time() - t0))
inst.close()
