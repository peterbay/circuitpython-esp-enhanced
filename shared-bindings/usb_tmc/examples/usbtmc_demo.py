"""A small SCPI instrument on the USBTMC interface.

boot.py::

    import usb_tmc
    usb_tmc.enable()

code.py::

    import usbtmc_demo
    usbtmc_demo.serve()

The host sees an instrument that answers ``*IDN?``, measures the chip
temperature and uptime, switches an output, keeps an error queue and the
IEEE 488.2 status registers. ``visa_client.py`` next to this file talks to it
from a PC with pyvisa.

Commands (case-insensitive, one per message, a ``?`` makes it a query)::

    *IDN?            identification
    *RST             reset: output off, errors cleared
    *CLS             clear the status registers and the error queue
    *ESR?            event status register, cleared by reading
    *ESE <n>/*ESE?   event status enable
    *OPC?            operation complete, always "1"
    MEAS:TEMP?       chip temperature in degrees C
    MEAS:TIME?       seconds since power-up
    OUTP ON|OFF|1|0  output on or off (the board LED when there is one)
    OUTP?            output state, "1" or "0"
    SYST:ERR?        oldest error as "<code>,\"<text>\"", or 0,"No error"
"""

import time

import microcontroller
import usb_tmc

try:
    import board
    import digitalio
    _led = digitalio.DigitalInOut(board.LED)
    _led.direction = digitalio.Direction.OUTPUT
except (ImportError, AttributeError):
    _led = None

IDN = b"CircuitPython,USBTMC demo,0,1.0"

# IEEE 488.2 event status register bits, and the status byte's summary bit
ESR_OPC = 0x01
ESR_QUERY_ERROR = 0x04
ESR_EXEC_ERROR = 0x10
ESR_COMMAND_ERROR = 0x20
STB_ESB = 0x20
STB_ERROR_QUEUE = 0x04

_errors = []
_esr = 0
_ese = 0
_output = False


def _error(code, text, esr_bit):
    global _esr
    if len(_errors) < 10:
        _errors.append((code, text))
    _esr |= esr_bit
    _update_status()


def _update_status():
    stb = 0
    if _errors:
        stb |= STB_ERROR_QUEUE
    if _esr & _ese:
        stb |= STB_ESB
    usb_tmc.set_status_byte(stb)


def _set_output(on):
    global _output
    _output = on
    if _led is not None:
        _led.value = on


def _reset():
    global _esr
    _set_output(False)
    _errors.clear()
    _esr = 0
    _update_status()


def handle(message):
    """Runs one command message. Returns the response, or None for a command."""
    global _esr, _ese
    text = message.strip().decode("ascii", "replace")
    parts = text.split(None, 1)
    if not parts:
        return None
    head = parts[0].upper()
    arg = parts[1].strip() if len(parts) > 1 else None
    query = head.endswith("?")
    if query:
        head = head[:-1]

    if head == "*IDN" and query:
        return IDN
    if head == "*RST" and not query:
        _reset()
        return None
    if head == "*CLS" and not query:
        _errors.clear()
        _esr = 0
        _update_status()
        return None
    if head == "*ESR" and query:
        value = _esr
        _esr = 0
        _update_status()
        return b"%d" % value
    if head == "*ESE":
        if query:
            return b"%d" % _ese
        try:
            _ese = int(arg) & 0xFF
        except (TypeError, ValueError):
            _error(-104, "Data type error", ESR_COMMAND_ERROR)
            return None
        _update_status()
        return None
    if head == "*OPC" and query:
        _esr |= ESR_OPC
        return b"1"
    if head in ("MEAS:TEMP", "MEASURE:TEMPERATURE") and query:
        return b"%.2f" % microcontroller.cpu.temperature
    if head in ("MEAS:TIME", "MEASURE:TIME") and query:
        return b"%.3f" % time.monotonic()
    if head in ("OUTP", "OUTPUT"):
        if query:
            return b"1" if _output else b"0"
        if arg is not None and arg.upper() in ("ON", "1"):
            _set_output(True)
        elif arg is not None and arg.upper() in ("OFF", "0"):
            _set_output(False)
        else:
            _error(-224, "Illegal parameter value", ESR_EXEC_ERROR)
        return None
    if head in ("SYST:ERR", "SYSTEM:ERROR") and query:
        if _errors:
            code, msg = _errors.pop(0)
            _update_status()
            return b'%d,"%s"' % (code, msg)
        return b'0,"No error"'

    _error(-113, "Undefined header", ESR_COMMAND_ERROR)
    if query:
        # A query nobody answers would leave the host waiting for its timeout.
        _error(-420, "Query UNTERMINATED", ESR_QUERY_ERROR)
    return None


def serve(idle=None):
    """Answers the host until interrupted. ``idle``, if given, is called with
    nothing pending, so the rest of code.py gets its turn."""
    if not usb_tmc.is_enabled():
        raise RuntimeError("usb_tmc.enable() is missing from boot.py")
    _reset()
    print("USBTMC demo instrument serving")
    while True:
        message = usb_tmc.read(timeout=0)
        if message is None:
            if idle is not None:
                idle()
            else:
                time.sleep(0.001)
            continue
        response = handle(message)
        if response is not None:
            # write() waits for the host to collect an earlier response; a
            # host that asked twice without reading gets this one when it does.
            usb_tmc.write(response + b"\n")
