# Fork changes

Modifications to CircuitPython for the M5Stack Cardputer (ESP32-S3, Xtensa LX7
windowed ABI, 240 MHz, 8 MB flash, no PSRAM). The work began on 10.2.1; the base
is now `adafruit/main` merged at `10.3.0-rc.0`, which section 9 describes.
Section 10 covers the audit of `shared-bindings` and `shared-module` that followed
it, and `AUDIT-FINDINGS.md` section 14 carries the finding-by-finding record.

Two areas: the display path and the bytecode interpreter. Everything measurable is
measured on the board; every number below comes from a run, not an estimate.

Every change is behind a config option that defaults to off upstream and is turned
on in `ports/espressif/mpconfigport.h`, so the whole set can be reverted by
flipping flags.

---

## 0. Things that cost time to find

Behaviour that is not obvious and that wasted an afternoon each. Read this before
debugging something that "should work".

**`boot.py` only runs on a hard reset.** Not on Ctrl-D, not on auto-reload after
saving a file. Editing `boot.py` and reloading appears to do nothing at all.
Power-cycle, press reset, or call `microcontroller.reset()`. Only `code.py` is
re-run by a soft reload.

**`supervisor.runtime.usb_connected` is always False inside `boot.py`**, even with
a host attached, because USB enumerates after boot.py finishes. The same call in
`code.py` returns True. So it cannot be used to decide anything about USB in the
one place USB decisions have to be made — use a marker file on CIRCUITPY or a
button held at boot instead.

**A FAT filesystem may only be written by one side at a time.** CircuitPython
enforces this so that the host and the device cannot corrupt it between them. The
root CIRCUITPY drive is read-only to Python whenever USB mass storage is up; a
partition mounted with `espidf.expose_partition(usb_writable=...)` picks a side at
boot. There is no "both" — logging to a drive and reading it live from the host at
the same time is not possible, the direction has to be switched and the board
restarted.

**A bad USB descriptor locks you out of the board entirely.** If the composite
descriptor is something the host refuses (Windows: "This device cannot start",
problem code 10), the serial port and the CIRCUITPY drive both disappear with it,
so there is no software route back in — not even safe mode, because reaching it
needs USB. The only way is ROM download mode: hold G0, tap reset, release G0.
Anything that changes the USB descriptor set (`usb_video`, `usb_vendor`, extra
HID devices) can do this, so be ready to reach the buttons before flashing it.

**A network round trip that is sometimes 15 ms and sometimes 120 ms is usually the
radio, not the code.** `wifi.radio.power_management` defaults to `MIN`, so between
packets the radio sleeps until the next DTIM beacon and an arriving request waits
for it. The wait is unpredictable because it depends where in the beacon cycle the
packet lands. Anything that measures a server's own handling will look innocent,
because it is: the time is spent before the first byte reaches the application.
Section 5 has the numbers and `espidf.wifi_sleep_min_active_time()` the dial.

**A generated `sdkconfig` cannot be reverted by editing the fragment it came
from.** `ports/espressif/build-*/esp-idf/sdkconfig` is itself an input to the
config system, and for a Kconfig `choice` it wins over the board's
`sdkconfig` fragment. Adding a line to the fragment takes effect; removing it
again does not, and neither does setting the opposite value — the build reports
success and silently keeps the old setting. The only way back is to delete the
generated file and let it regenerate. Check the value there, not in the
fragment, before trusting any measurement taken across such a change.

**Benchmarks run back to back in one REPL session are not comparable.** Every
`exec()` leaves its module-level objects behind, so the heap grows, fragments,
and eventually refuses a large contiguous allocation: several test suites here
raise `MemoryError` at the end of a long chain and pass on their own. It also
moves timings — `gc.collect()` on an idle heap measured 0.58 ms on a freshly
reset board and 1.01 ms after a few runs. Reset the board between measurements
that are meant to be compared, and treat `gc.mem_free()` as a property of the
session rather than of the firmware.

**The version string on the board can name an older release than the tree
actually contains.** `py/version.py` builds it with `git describe --first-parent`,
so the walk stays on this branch's own chain of commits and never crosses into a
merge's second parent. A tag that arrived through a merge from `adafruit/main` is
therefore invisible to it. After the merge described in section 9 the board
reported `10.3.0-alpha.4-119-g31a146df91` while plain `git describe` on the same
commit said `10.3.0-rc.0-74-g31a146df91` — same commit, older tag, and a distance
inflated because it is being measured from further back. The hash after the `g` is
the part to trust; the tag in front of it only says where this branch last
diverged, not what has since been merged in.

Three more from elsewhere in this document, worth repeating here:

- **Never probe a partition by erasing it.** An erase to find out whether a
  partition is writable destroys the boot sector of a filesystem living there.
  Use `espidf.running_partition()` to find the one to avoid.
- **`__setattr__` used to be silently ignored** at this build's ROM level. It is
  enabled now (`MICROPY_PY_DELATTR_SETATTR`), so a class that defines it and
  previously appeared to work is now actually intercepting attribute writes.
- **`__setitem__`, `__delitem__` and `__setattr__` are not exposed as attributes**
  in MicroPython even when they work: `obj.__setitem__(k, v)` raises
  `AttributeError` while `obj[k] = v` is fine. This bites when writing tests.

---

## 1. Interpreter

### New config options

All default to `0` in `py/mpconfig.h` and are set to `1` for this port.

| Option | What it does |
| --- | --- |
| `MICROPY_NLR_SETJMP_BUILTIN` | `__builtin_setjmp` instead of the libc one for the non-local return buffer |
| `MICROPY_OPT_LOAD_GLOBAL_CACHE` | Remembers name → builtin resolutions |
| `MICROPY_OPT_LOAD_GLOBAL_CACHE_SIZE` | Entries in that cache (32) |
| `MICROPY_OPT_LOAD_METHOD_FAST_PATH` | Resolves a method on a class instance in the VM |
| `MICROPY_OPT_CALL_FUN_BC_FAST_PATH` | Calls a bytecode function straight from the VM |
| `MICROPY_OPT_CALL_BUILTIN_FAST_PATH` | Calls a builtin straight from the VM |
| `MICROPY_OPT_BYTEARRAY_SUBSCR_FAST_PATH` | `bytearray[i]` read and write in the VM |
| `MICROPY_OPT_BYTES_SUBSCR_FAST_PATH` | `bytes[i]` read in the VM. Defaults to whatever the bytearray option is set to |
| `MICROPY_OPT_DICT_SUBSCR_FAST_PATH` | `d[k]` read and write in the VM |
| `MICROPY_OPT_ITERNEXT_FAST_PATH` | Calls the iternext slot straight from the VM |
| `MICROPY_OPT_TRUTH_FAST_PATH` | Decides truth of the common containers in the VM |
| `MICROPY_OPT_STR_NO_INTERN` | Does not search the qstr pools for a newly built string |
| `MICROPY_OPT_SINGLE_CHAR_QSTR_CACHE` | Remembers the qstr of each ASCII character |
| `CIRCUITPY_DISPLAY_DOUBLE_BUFFER` | Composes the next subrectangle while the previous one is on the bus. **Off** by default, see section 2 |
| `CIRCUITPY_DISPLAY_AREA_BUFFER_SIZE` | Size of one area buffer (512 B upstream, 4096 here) |
| `CIRCUITPY_ESPIDF_CSI` | Builds `espidf.CSI`, needs `CONFIG_ESP_WIFI_CSI_ENABLED` |
| `ESPIDF_CSI_MAX_BYTES` | Channel data kept per CSI record (128) |
| `CIRCUITPY_REGISTERS` | Builds the `registers` bit-field module (default: full build) |

### `py/vm.c`

The dispatch loop was split: `mp_execute_bytecode_loop()` holds the loop and
`mp_execute_bytecode()` is a small wrapper that owns the `nlr_push` and the
exception handler. `RAISE()` became a plain `nlr_raise()`, three live `nlr_pop()`
calls left the loop, and the exception handler returns with `continue` instead of
a jump back into the loop.

Fast paths added to the opcodes:

- `LOAD_SUBSCR` / `STORE_SUBSCR`: list, tuple, bytearray, dict, and `bytes` for
  reads. Exact type comparison, so subclasses keep their overrides. A miss falls
  through so the general path still reports `IndexError`, `KeyError` or the
  overflow. **Negative indices stay on the fast path**: they used to be excluded
  by an `i >= 0` gate, so `x[-1]` — ordinary Python — took the general route at
  342 cycles against 111 for `x[0]`, and `bytearray[-1] = v` at 448. Each arm now
  normalises the index against its own length before the bound test; an index
  below `-len` wraps to a large `size_t`, fails that test and still reaches
  `mp_get_index` for the `IndexError`. Reads and writes both drop to the cost of
  a positive index, the positive path does not move, and the firmware got 32
  bytes smaller. `bytes` was the odd one out for a long time — with
  `MICROPY_PY_BUILTINS_STR_UNICODE` set, `bytes_subscr()`'s type test folds to
  compile-time true and it returns `MP_OBJ_NEW_SMALL_INT(data[i])`, which is what
  the fast path produces by a shorter route. **2492 ns → 1526 ns**, which is the
  same index on a `bytearray` to within noise.
- `BINARY_OP_MULTI`: both operands small ints. Comparisons, `+`, `-`, `&`, `|`,
  `^`, `*`, `//`, `%`, `<<`, `>>`. The bitwise ones need no unpacking at all: with
  the tag in the low bit, `and` and `or` keep it set and `xor` only has to put it
  back, and the result always fits because a small int has its top two bits equal.
- `CALL_FUNCTION` / `CALL_METHOD`: a bytecode function is called through
  `mp_obj_fun_bc_call()` directly, and a builtin through `vm_call_builtin()`,
  which dispatches on `mp_type_fun_builtin_0..3` and `_var` and calls the C
  function itself.
- `LOAD_METHOD` and `STORE_ATTR` on class instances.
- `FOR_ITER`: calls the type's iternext slot when the type says its `iter` slot
  really is iternext and it is not a stream.
- `POP_JUMP_IF_*` and `JUMP_IF_*_OR_POP`: `vm_is_true()` settles small ints,
  `None`, `bool`, list, tuple, dict, str and bytes inline.

### `py/nlr.h`, `py/nlrsetjmp.c`

`MICROPY_NLR_SETJMP_BUILTIN` swaps the libc `setjmp`/`longjmp` for the compiler's
own non-local goto. On a core with register windows the libc version flushes the
whole register file on every `nlr_push`, which is every Python call; the compiler
version saves the frame and defers the flush to the jump, which only happens when
an exception is really raised. **305 cycles against 6.**

The buffer becomes `void *jmpbuf[5]` instead of `jmp_buf`. `nlr.h` rejects the
combination with the native emitter at compile time, see section 6.

### `py/runtime.c`, `py/map.c`, `py/objdict.c`, `py/obj.h`

`mp_load_global()` keeps 32 entries of `{name, globals, mutation, value}`. A
builtin name costs two map lookups, and the first always misses. Only results
that come from the const builtins table are cached, and the cache is bypassed
when `mp_module_builtins_override_dict` is set.

Correctness rests on `mp_map_mutation_count`, a counter bumped at every place a
map gains or loses a key: four sites in `mp_map_lookup()`, plus `mp_map_clear()`,
`mp_map_deinit()`, the memcpy path in `mp_obj_dict_copy()` and `dict_popitem()`.
Any change to any map invalidates every entry, which is blunt but cannot go stale.

### `py/objstr.c`, `py/objstrunicode.c`, `py/qstr.c`, `py/qstr.h`, `py/modbuiltins.c`

Two separate things, both about the qstr pools.

**Newly built strings are no longer interned.** `mp_obj_new_str()` and
`mp_obj_new_str_type_from_vstr()` used to call `qstr_find_strn()`, which binary
searches the ROM pool of about 2000 entries with every step reading a string out
of flash through the cache, and then walks the run-time pool linearly because it
is not sorted. Measured at ~4400 cycles, independent of the string length, and it
only pays off when the computed data happens to equal a name the firmware already
knows. The empty string is still returned as a qstr because it is common and free.

**The qstr of each ASCII character is remembered** in a 128-entry table in
`qstr.c`, used by `str_subscr()`, `str_it_iternext()`, `chr()` and the two
byte-string paths. Indexing a string and iterating one both produce a single
character, and looking it up cost between 300 and 1400 cycles depending on where
that character happened to sit in the pools. `qstr_reset()` clears the table with
the pools.

A character above ASCII is no longer turned into a qstr at all. It used to become
one permanently: 2000 different characters cost about 18 kB that never came back,
and because the run-time pool is scanned linearly every later lookup got slower.
**125 µs per character against 3 µs now**, and the memory is collectable.

Cost: `"a" + "b" is "ab"` and `chr(0x10d) is "č"` are now `False`, which CPython
does not guarantee either, and equal strings no longer share memory. Equality,
hashing, dict lookup, `getattr` and globals all handle non-interned strings —
`mp_map_lookup()` falls back to a full comparison for them, see `py/map.c:186`.

### `py/objstr.c` — string formatting and payload allocation

**A format field with no spec no longer builds a string to throw away.** The
lexer rewrites every `f"..."` into `"...".format(...)` with bare `{}` fields, and
such a field had its text built into a vstr, converted into a real `str` object
with a hash nothing reads, and taken apart again a few hundred lines further
down. With no spec the rest of that loop body reduces to printing the text
unpadded and untruncated — `width` stays `-1`, so `mp_print_strn()`'s pad comes
out negative — so it can go straight into the output vstr instead.

The conversion itself has to stay. Skipping it leaves an int as the argument and
`arg_looks_numeric()` at `objstr.c:1383` then routes it into `mp_print_float()`
with type `'g'`, turning `f"{1234567}"` into `"1.23457e+06"`. That was the first
version of this change and it was wrong in exactly the way that produces silently
bad output rather than a crash.

**`"{}".format(str)` 14709 → 9430 ns, `f"{str}{int}"` 25177 → 13255 ns.** The
path with a format spec is untouched — and is, for the same reason, the faster
of the two before the change.

**String payloads are no longer scanned by the collector.** `m_new()` sets the
collect bit, so every live string's bytes were walked word by word looking for
heap pointers they cannot contain. CircuitPython already has
`m_malloc_without_collect()` for exactly this and uses it for `objarray` and
`qstr` leaf buffers; strings were missed. This shortens GC pauses only, and it
covers `mp_obj_new_str_copy()` — the vstr path used by `+`, `%`, `.format()` and
`str(int)` still allocates a collectable buffer, which is a much larger surface.

### `py/objtype.c`

`mp_convert_member_lookup()` already calls a native property's fixed-arity
builtin getter directly, but a Python subclass of a native type never reaches
it: `mp_obj_class_lookup()` hands the property object back raw (a CircuitPython
divergence, `objtype.c:199`), so `mp_obj_instance_load_attr()` and
`_store_attr()` are entered instead and both went through
`mp_call_function_n_kw()`. Because the native base's
`MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS` is inherited, both VM fast paths are
disqualified as well, so *every* attribute access on such a class takes the long
route. This is the `class X(displayio.Group)` pattern the display libraries are
built on.

The `n_proxy` guard stays first in the setter: under
`MICROPY_PY_OPTIMIZE_PROPERTY_FLASH_SIZE` a getter-only property is a shorter
struct and `proxy[1]` is out of bounds.

On a `displayio.Group` subclass: reading `.x` **5056 → 4415 ns**, `.hidden`
**5015 → 4405 ns**, writing `.x` **4354 → 3916 ns**. The plain native path is
unchanged at 2543 ns. The remaining gap to it is the `mp_obj_class_lookup()`
walk, which this does not touch.

### `py/gc.c`

Two bugs, both making the collector scan memory it did not need to.

`gc_deal_with_stack_overflow()` walked every block of every area including the
tail that has never been allocated, while `gc_sweep_free_blocks()` seventy lines
below correctly bounds the same walk by `gc_last_used_block` and asserts the
bound. With `MICROPY_GC_SPLIT_HEAP_AUTO` a freshly grown area holds one
allocation behind a full-size allocation table, so the tail is most of it. This
only runs after the 128-entry mark stack overflows — which, it turns out, is not
rare.

On the not-found path of `gc_alloc()`, `i` is still an allocation-table *byte*
index; it becomes a *block* index only on the `found` path. The same expression
is therefore correct at the end of that function and wrong here, where it stored
`byte_len/4` and left the cursor a quarter of the way into an area that had just
been proven full. Every later single-block allocation rescanned 75 % of it. The
comment above the line says the intent is to mark the area filled, and
`gc_free()`'s own comment already assumes filled areas are skipped.

Together with the string payload change above, best of seven collections: an
idle heap **3.60 ms → under 1.01 ms**, a heap holding 64 kB of live `bytes`
**14.65 ms → 1.46 ms**. Before, the same volume of `bytearray` cost 0.98 ms
against `bytes`'s 11.05 ms over an empty heap; after, the two are equal.

Then a third change, to the mark phase itself. `gc_get_ptr_area()` is asked about
**every word of every scanned block**, and it walked the linked list of heap areas
for each one. The alignment test at its head already turns away small ints and
qstrs, whose tags leave them unaligned, but a zero word — of which a heap has many
— or a pointer into ROM passes that test and then walked the whole list to find
nothing. Keeping the lowest `gc_pool_start` and highest `gc_pool_end` across all
areas settles those in two comparisons. This is MicroPython 1.29's
`area_pool_min`/`area_pool_max`, ported.

The bounds are **recomputed**, not merely widened, because `MICROPY_GC_SPLIT_HEAP_AUTO`
hands an empty area back in `gc_sweep_free_blocks()`; a span left covering memory
that is no longer ours would send freed addresses into the list walk. They are
refreshed in `gc_init()`, in `gc_add()` and at that removal site.

Marking got about **1.4× faster** on a pointer-heavy heap: 1500 small lists
5828 → 4150 µs, and per object above the mark-stack limit 3.70 → 2.63 µs. A heap
of `bytearray` payload does not move, which is the expected shape — that payload
is allocated without the collect bit and was never scanned in the first place.

### `py/stream.c`

`stream_unbuffered_readline()` asked the stream for **one character at a time**,
which on FAT is one `f_read()` per character. Its own comment called it
"unbuffered, inefficient", and `readline()`, `readlines()` and `for line in f`
all funnel through it, so all three paid.

A stream that can seek is now read in 64-byte blocks, scanned with `memchr()`,
and rewound past whatever followed the newline. Seekability is probed with a
zero-offset `MP_STREAM_SEEK`; UART, stdio and sockets fail it and keep the
byte-at-a-time loop, which is why this belongs in the generic function rather
than in the FAT file object.

Over a 17892-byte file, board reset between runs: `for line in f` **28961 →
11352 µs** in text mode and **26275 → 9307 µs** in binary, `readline()`
**29663 → 12023 µs**, `readlines()` **28655 → 10314 µs**. Binary iteration now
costs the same as `read()` of the whole file, which is the floor.

### `py/cstack.h`, `py/cstack.c`, `py/pystack.h`, `py/pystack.c`, `py/bc.c`, `py/objfun.c/.h`

Three out-of-line calls per Python call removed:

- `mp_cstack_check()` is now `static inline` in the header. The depth is measured
  in the caller's frame instead of inside `mp_cstack_usage()`, so the reported
  usage is one leaf frame lower.
- `mp_pystack_alloc()` is inline; only `mp_pystack_alloc_fail()` stays out of line.
- `mp_setup_code_state_helper()` has a fast path for the plain case (no keywords,
  no varargs, argument count matches) and memsets only the slots that are not
  arguments.

`fun_bc_call()` was `static`; it is now `mp_obj_fun_bc_call()` and declared in
`objfun.h` so the VM can call it without the type-slot indirection.

### `py/objrange.c`

`range_it_iternext()` used to call `mp_obj_new_int()` out of line on every step.
The value almost always fits in a small int, so that test is now inline.

Note that `for i in range(...)` written literally never reaches this: the compiler
turns it into a counted loop, see `compile_for_stmt_optimised_range()` in
`py/compile.c:1431`. It applies when a `range` object is iterated from a variable.

### `ports/espressif/mpconfigport.h`, `supervisor/shared/background_callback.c`

`MICROPY_VM_HOOK_LOOP` called `background_callback_run_all()` on every branch,
which unconditionally called the empty `port_background_task()` before even
looking at the queue. It now tests `callback_head != NULL` inline and pays for the
call only when there is work. Same semantics, same latency, one load instead of
two out-of-line calls. `callback_head` lost its `static` for this.

---

## 2. Display path

`shared-module/busdisplay/BusDisplay.c` — when the refreshed area fits in one
chunk, the window is set once and the following chunks continue with
`MIPI_COMMAND_WRITE_MEMORY_CONTINUE` (0x3C) instead of repeating the full column
and row address setup per chunk. Added to
`shared-module/displayio/mipi_constants.h`.

`shared-module/displayio/TileGrid.c` — `tilegrid_fill_area_fast()`, a per-format
specialised fill that tabulates the palette into a lookup table, walks the
transparency mask by words and takes shortcuts when the whole run is opaque or
has no mask.

It began as a single-tile path, which meant text never reached it. It now takes
multi-tile grids through a two-level walk. The outer level steps whole tiles and
does the tile lookup and its two divisions once per crossing; the inner level runs
along one fixed bitmap row. A run is cut at whichever boundary comes first, the
tile's or the mask word's, so the inner body never has to ask which tile it is in.
Because a tile boundary can now cut a run short of the mask word, the run's own
bits are tested and merged rather than the word being tested and overwritten —
otherwise the second tile in a word would find the word non-zero and drop every
remaining pixel of that word onto the slow per-pixel branch.

Out of range reads return 0 from `common_hal_displayio_bitmap_get_pixel`, and 0 is
a real colour index, so the general loop silently shades such pixels with palette
entry 0. Rather than put a bounds test back into the inner run, `tilegrid_tiles_fit`
checks once per call that every tile the dirty area touches addresses a window
wholly inside the bitmap, and hands anything else to the general loop. The check
walks only the tile columns and rows the area reaches, so it costs one byte compare
per touched tile — around a thousand cycles against a 2 ms full-screen fill.
It is kept even though both Python routes to a tile value are now validated,
because `common_hal_displayio_tilegrid_set_tile` is also called from C —
terminalio passes glyph indices straight from the font — and those callers bypass
the binding's range check.

Group scale above 1 still goes to the general loop. Each source pixel would then
repeat `scale` times and the run segmentation stops being a straight copy; the
terminal's `CIRCUITPY_TERMINAL_SCALE` defaults to 1, so this is not on the text path.

The general loop that everything else falls back to did nine divisions per pixel.
Only two of them can change between neighbouring pixels, and even those change at
most once per tile column, so the column is now walked with counters
(`tilegrid_x_walk_t`): a phase counter for the group scale, an offset within the
tile, and the tile index itself. The tile lookup and its origin in the bitmap are
refreshed only when the column crosses a tile boundary, and the row parts —
`y_tile_index`, the row's tile base, the offset within the tile row — are hoisted
out of the x loop. `start_x` is a transformed overlap measured from
`current_area` and never negative, so truncation matches floor and the counters
are exact. The grid geometry is read into locals once instead of chasing `self`
through two pointers per pixel.

`CIRCUITPY_TILEGRID_VERIFY_FASTPATH` builds a checking firmware that does both
comparisons. The general loop recomputes the original per-pixel expressions and
compares them against the walked counters; and the fast path runs into shadow
buffers while the general loop still produces the real output, after which the two
are compared pixel by pixel along with the mask and the coverage result.

Two traps there. The first version printed differences from inside the loop, and
since the console is the display terminal, that scrolled the very TileGrid being
rendered, changed `top_left_y` under the comparison and produced hundreds of
mismatches of its own; it records the first difference and reports it after the
loop. And the shadow buffers were originally 2048 pixels, so a full-screen refresh
was quietly skipped rather than compared — a passing run would then have meant only
that small areas were right. They now hold a whole 240x135 screen, and the count of
calls that went unverified is printed alongside the others.

Clean over 800k checked pixels and 600 whole-area comparisons with **none
unverified**, across terminal scrolling, tile sizes from 3x9 to 32x24, tile widths
that are not byte multiples at 1 and 2 bits per pixel, scales 1 to 3, all flip and
transpose combinations, transparent palette entries under overlapping layers,
bitmap depths of 1 to 16 bits, a bitmap of 320 tiles, grid offsets that wrap
`top_left` past the edge, and tiles pointed outside the bitmap.

`ports/espressif/common-hal/busio/SPI.c` — transfers of four bytes or fewer use
`spi_device_polling_transmit()`, which skips the interrupt and the task switch.

`ports/espressif/boards/m5stack_cardputer/` — SPI at 80 MHz in `board.c` and
`CIRCUITPY_DISPLAY_AREA_BUFFER_SIZE (4096)` in `mpconfigboard.h`.

`ports/espressif/common-hal/paralleldisplaybus/ParallelBus.c` — larger DMA
transfer limit and a deeper transaction queue.

Full-screen refresh went **89.8 ms → 17.1 ms** at this point. The three changes
described further down took it to **8.8 ms**.

Multi-tile grids. Average of 20 full redraws, forced by moving the grid one pixel
so both the old and the new position are dirty. Every number includes the SPI
transfer, so the first row is the floor: a single tile covering the whole screen,
which took the fast path in all three columns and therefore measures the transfer
plus a minimal fill.

| | original | divisions hoisted | multi-tile fast path |
| --- | --- | --- | --- |
| One tile of 240x135 (floor) | — | — | 12.7 ms |
| Terminal, 40x11 tiles of 6x12 | 43.4 ms | 39.4 ms | **14.9 ms** |
| Sprites, 30x16 tiles of 8x8 | 42.2 ms | 37.9 ms | **13.7 ms** |
| Sprites, 60x33 tiles of 4x4 | 43.6 ms | 39.6 ms | **15.9 ms** |
| Sprites, 15x8 of 8x8 at scale 2 | 82.9 ms | 74.2 ms | 75.0 ms |

Hoisting the divisions was worth about 10%, and that is all they were worth — the
measured cost of an integer division on this chip is under five cycles. What
remained was dominated by the per-pixel call to
`common_hal_displayio_bitmap_get_pixel()` and the palette lookup, which is what the
fast path avoids by tabulating and reading rows directly. Against the hoisted
general loop it is **2.5x to 2.8x** on the complete refresh; measured against the
floor, the fill itself drops from 25-27 ms to 1-3 ms. Narrow tiles keep more of the
cost because a 4x4 grid crosses a tile boundary every four pixels and pays the
lookup each time. The scale 2 row is unchanged because it still falls back, and it
is the honest reminder that the two columns either side of it are not measuring the
same code.

### Overlapping composition with transmission

Once the fill was fast, the frame was bounded by the bus. Profiling a full-screen
refresh with `CIRCUITPY_PROF=1` split it as:

| | ms | share |
| --- | ---: | ---: |
| `send_pixels` | 7.86 | 58% |
| `fill_area` | 4.86 | 36% |
| `set_region` | 0.17 | 1% |
| the rest of `_refresh_area` | 0.77 | 6% |

with 7.24 ms of that transfer being the bytes actually on the wire at the measured
71.6 Mbit/s. The two legs ran strictly one after the other: compose a subrectangle,
hand it to the bus, block until it is out, compose the next.

`CIRCUITPY_DISPLAY_DOUBLE_BUFFER=1` composes subrectangle *j* **before** waiting
for the transfer of *j-1*, so the composition hides inside the transfer. Two
buffers, because the DMA is still reading the previous one.

```
compose A            start send(A)
compose B  | send(A) wait A, start send(B)
compose A  | send(B) wait B, start send(A)
```

Exactly two are needed and a third would not help: the bus is serial, so nothing
beyond one outstanding transfer can be in flight.

What it touches:

- `ports/espressif/common-hal/busio/SPI.c/.h` — `common_hal_busio_spi_write_async()`
  queues one transaction and returns; `common_hal_busio_spi_wait_async()` collects
  it. The `spi_transaction_t` had to move into `busio_spi_obj_t` because the driver
  holds the pointer until the result is collected, so it cannot live on the stack of
  the call that queued it. `common_hal_busio_spi_transfer()` drains any outstanding
  transfer first, because its short path polls and ESP-IDF forbids polling while
  anything sits in the queue — without that, `sdcardio` or USB MSC touching the same
  bus mid-refresh would break.
- `shared-module/fourwire/FourWire.c` — `send_async()` only hands over plain pixel
  data with a real D/C pin and no per-byte chip select; the nine bit emulation and
  the toggling variants are sent the ordinary way and report that nothing is owed.
  `flush()` waits.
- `shared-module/displayio/bus_core.h/.c` — an optional `send_async` pointer, NULL
  on buses that cannot do it, which then keep the old path. `flush` was already in
  the interface for `qspibus`.
- `shared-module/busdisplay/BusDisplay.c` — the restructured loop.

Two things it is easy to get wrong. `end_transaction()` has to come **after** the
flush, not straight after the send: chip select is a GPIO driven by FourWire and
dropping it while the DMA is still clocking would cut the tail off the
subrectangle. And a refused queue must not set the pending flag, or the next
iteration waits for something that is not there — the same class of hang the
`spi_device_queue_trans()` return value fix in section 4 was about.

### Two smaller things the profiler found

**The window is only reprogrammed when it moves.** `set_region` is four small bus
operations, and each costs more in ESP-IDF driver overhead than the ten bytes it
carries — 169 us measured. An application redrawing the same rectangle every frame
paid that every time. `displayio_display_bus_t` now remembers the last window and
skips the commands when it matches: **169 us → 5 us**. Safe because every pixel
write starts with `WRITE_MEMORY_START`, which puts the controller's address pointer
back at the window origin, so only the rectangle itself has to be tracked. Guarded
by `CIRCUITPY_DISPLAY_LIMIT <= 1`, since two displays on one bus keep separate
copies of this state and would each believe its own window still stood.
`displayio_display_bus_forget_region()` clears it after an init sequence, which is
free to program the address registers itself.

**Subrectangle rows are spread evenly.** The old split filled each buffer and left
whatever remained as a tail: a 38 row area with room for 34 became 34 and 4. With
the transfer of one overlapping the composition of the next, a four row tail
composes in almost no time and therefore hides none of the 34 row transfer, so that
wait is paid in full. Two 19 row halves cost the same to compose and to send but
overlap properly: **`chunk_bus` 311 us → 68 us**. The change only ever lowers
`rows_per_buffer`, so the buffer stays large enough, and for a full screen (135 rows
at 8 per buffer) it computes the same 8 and changes nothing.

### What this ended up worth

Same benchmark as above. The middle column is 80 MHz plus the 4 kB area buffer;
the right column adds double buffering.

| | 40 MHz, 512 B | 80 MHz, 4 kB | + double buffered |
| --- | ---: | ---: | ---: |
| One tile of 240x135 (floor) | 30.2 ms | 13.8 ms | **8.8 ms** |
| Terminal, 40x11 tiles of 6x12 | 32.4 ms | 16.1 ms | **10.0 ms** |
| Sprites, 30x16 tiles of 8x8 | 31.0 ms | 14.9 ms | **8.9 ms** |
| Sprites, 60x33 tiles of 4x4 | 33.8 ms | 17.2 ms | **11.2 ms** |
| Sprites, 15x8 of 8x8 at scale 2 | 112.8 ms | 76.7 ms | 66.5 ms |

**3.4x on the full-screen floor**, of which 2.2x is the clock and the buffer and
1.6x is the overlap. Predicted 1.55x for the overlap and measured 1.54x, which is
the only estimate in this document that landed.

The scale 2 row moves only 1.15x, and that is the model working rather than
failing: it is bound by composition, not by the bus, so there is only the 7.9 ms of
transfer to hide behind. Extending the fast path to integer scale is what that row
needs, and it is the largest single number left in the display path.

Small-area refreshes, which is what a real UI does, went **1613 us → 1316 us** for a
60x38 dirty rectangle across the window cache and the row balancing.

### Buffer size, and why not larger

The area buffers are VLAs on the CircuitPython task's stack, which is 24 kB. One
4 kB buffer is 4084 bytes with its mask; two are 7924.

| | pixel buffers | stack | full-screen frame |
| --- | ---: | ---: | ---: |
| one 4 kB, synchronous | 3840 B | 4084 B | 13465 us |
| **two 2 kB** | 3840 B | 3964 B | 10118 us |
| **two 4 kB** | 7680 B | 7924 B | **8740 us** |

Two 2 kB buffers give the overlap for **no extra memory at all** — the halves
together are the size of the single buffer, and the mask is smaller. It costs 1.38
ms because the subrectangles double from 17 to 34 and each one carries about 80 us
of fixed cost on both sides, in the queueing and in the per-call setup of
`fill_area`. An earlier guess of 0.65 ms for that, derived only from the 36 us of
measured per-chunk SPI overhead, was half the real figure.

Two 4 kB is the configured default here. Going further means moving the buffers off
the stack; at 8 kB each they would take two thirds of it.

### A palette of three colours is slower than one of four

Not a change, a finding, and it costs more than it looks. The fast path skips the
per-pixel transparency and range tests only when **every value the bitmap can hold**
maps to an opaque colour. A `Bitmap(w, h, 3)` stores 2 bits per pixel, so
`(1 << 2) == 4 > 3` and the shortcut is off. `Palette(4)` with the fourth entry
unused turns it back on. The bitmap is the same size either way, since 3 and 4
values both need 2 bits.

| | `fill_area`, full screen | whole refresh |
| --- | ---: | ---: |
| `Palette(3)` | 7752 us | 10555 us |
| `Palette(4)` | **4821 us** | **9020 us** |

Composition 1.61x, the frame 1.17x. Worth knowing before designing a bitmap around
exactly three colours.

---

## 3. New Python API

### `espidf.Timer`

```python
import espidf

t = espidf.Timer(callback, 0.02)                 # period in seconds
t = espidf.Timer(callback, 0.5, repeat=False)    # one shot
t.stop(); t.start(); t.deinit()
t.active                                          # read only
```

Wraps `esp_timer`. The ESP-IDF callback runs in the timer's own task, where
nothing may touch the interpreter, so it only queues a background callback and the
Python function runs on the Python thread between two bytecodes.

**This is not an interrupt.** It cannot pre-empt running code and it is delayed by
anything that blocks without servicing background tasks. Measured with a 20 ms
period: mean interval 18.5 ms, max 20.0 ms, min 6.2 ms, the short ones being ticks
caught up after a block. Good enough for polling a sensor or repainting; not for
timing an output.

An exception inside the callback is printed and the timer keeps running.

At most 8 timers exist at once; the ninth raises `RuntimeError`. Running timers
are held in a list the collector traces, so they survive without the program
keeping a reference, and their handles are also kept outside the heap so soft
reset can take them down after the objects are gone.

### `espidf.Partition`

Raw access to the flash partition table. The point is `mmap()`: the partition is
mapped into the data address space and returned as a read-only memoryview, so
large tables can be read **without a single byte on the heap**. Measured: walking
200 kB through the mapping left `gc.mem_free()` unchanged at 146048.

```python
import espidf

espidf.partitions()          # [(label, type, subtype, address, size), ...]

p = espidf.Partition("ota_1")
p.label, p.size, p.address
p.erase(0, 4096)             # whole 4096 byte sectors only
p.write(0, data)             # the area has to be erased first
p.read(offset, length)
mv = p.mmap()                # read only memoryview over the whole partition
p.deinit()                   # releases the mapping
```

On this board `ota_1` is 2 MB at 0x210000 and unused, because OTA is not compiled
in (`CIRCUITPY_DUALBANK=0`). That makes it the obvious place for large read-only
data, with the caveat that turning OTA on later would collide.

Writing to the partition the firmware is running from is refused
(`esp_ota_get_running_partition()`). Nothing else is guarded: it is possible to
destroy the filesystem by writing to `user_fs`.

### `espidf.power_management()`

```python
espidf.power_management()                                  # reports (min, max, light_sleep)
espidf.power_management(min_frequency=80, max_frequency=240)
espidf.power_management(light_sleep=True)
```

Wraps `esp_pm_configure()`. `CONFIG_PM_ENABLE` was already on, needed for a
settable CPU frequency, but CircuitPython only ever set a fixed frequency. The
clock now drops to `min_frequency` whenever nothing holds a power management lock,
which is where the battery saving is.

The default is min 240 / max 240, so nothing scales until you ask for it.

**`light_sleep=True` is untested here** because it drops the USB connection, which
makes it unusable while developing over USB. It is passed straight through to the
IDF.

### `espidf.NVS`

Named key/value storage, the layer under `microcontroller.nvm`. That one gives a
single flat 8 kB array addressed by offset; this keeps typed values under names,
with wear levelling and atomic writes, the way the ESP-IDF stores it.

```python
nvs = espidf.NVS("settings")     # namespace, up to 15 chars, created if new
nvs["brightness"] = 128          # int (64-bit), str or bytes
nvs["ssid"] = "home"
nvs["cal"] = b"\x00\x01\x02"
nvs["brightness"]                # -> 128, typed as stored
nvs.keys()                       # every key in this namespace
del nvs["ssid"]
nvs.deinit()
```

Keys are at most 15 characters. Values persist across reset and reopening. A
missing key raises `KeyError`. Uses whatever namespace you pass, so it does not
collide with the `"CPY"` namespace `microcontroller.nvm` writes into.

### `hashlib` completed

`hashlib` had `new()` and nothing else: no named constructors, and a `Hash` object
carrying only `update`, `digest` and `digest_size`. Two consequences, and the second
is the expensive one.

`from hashlib import sha256` — the form portable libraries use — simply failed. And
`adafruit_hashlib`, which is installed on this board, asks for six names in a single
import, so one missing name dropped **every** algorithm in it, and everything built
on it, into its pure Python implementations.

Measured before the change, with the bundle's own code:

| SHA-256 of | pure Python | native |
| --- | --- | --- |
| 16 bytes | 34.4 ms | 82 us |
| 256 bytes | 168.6 ms | 100 us |
| 4096 bytes | **2.20 s** | 238 us |

Nine thousand times, and it grows with the input. The same audit pointed at
`adafruit_binascii`, which never imports the native `a2b_base64`/`b2a_base64` that
this firmware does have: Base64 of 256 bytes was 416x slower in Python, and at
4096 bytes the Python version **fails outright** with `MemoryError` trying to
allocate 32 kB. That one is a library fix, not a firmware one.

Added: `md5()`, `sha1()`, `sha224()`, `sha256()`, `sha384()`, `sha512()` as module
level constructors, and `hexdigest()`, `copy()`, `block_size` and `name` on the
object. `new()` accepts the four new names too. The driver behind it already
implements all of them, so this costs **1184 bytes of flash**.

Written against PSA Crypto. Upstream replaced the mbedtls backend of `hashlib`
with `psa/crypto.h` between 10.2 and 10.3, so the original version of this change
was thrown away and redone rather than merged. That turned out to be the cheaper
direction:

- `digest_size` is `PSA_HASH_LENGTH(alg)` and `block_size` is
  `PSA_HASH_BLOCK_LENGTH(alg)`, both one line instead of a table.
- `copy()` is `psa_hash_clone()` into an operation freshly returned by
  `psa_hash_operation_init()`, which is the inactive destination it requires.
- The truncation trap that the mbedtls version had to work around is gone.
  `mbedtls_sha256_finish` writes a full 32 bytes whatever the variant, so sha224
  finishing straight into a correctly sized destination ran 4 bytes past it and
  the code had to finish into a local buffer and copy the leading part.
  `psa_hash_finish()` takes the size of the destination, so there is nothing to
  get wrong.
- A build whose PSA configuration leaves an algorithm out answers
  `PSA_ERROR_NOT_SUPPORTED` from `psa_hash_setup()`, which `new()` reports as an
  unsupported algorithm rather than failing later.

On espressif the PSA implementation comes from the mbedtls inside ESP-IDF, not
from `lib/mbedtls` -- that submodule is not in this port's dependency list at all.

`copy()` exists because HMAC needs to fork a partially fed state, and because
`adafruit_hashlib` and `circuitpython_hmac` both check for it. The existing
`digest()` already cloned the operation so it could be called twice; the same
clone underpins `copy()`.

Verified against reference digests computed independently with CPython's `hashlib`:
131 checks over six algorithms × four input sizes × three entry points, plus the
property values, repeated `digest()`, feeding more after a digest, `copy()` diverging
correctly on every algorithm, and unknown names raising.

**`adafruit_hashlib` still falls back, and no firmware change can fix that.** Its
import block is

```python
from hashlib import md5, sha1, sha224, sha256, sha512
from hashlib import sha3_384 as sha384      # same try block
```

The first line now succeeds. The second cannot: `sha3_384` is Keccak, which the PSA
configuration ESP-IDF ships does not carry, and the library aliasing it to `sha384`
is a bug of its own — SHA3-384 and SHA-384 are
different algorithms. Confirmed on hardware: `ImportError: can't import name
sha3_384`, and splitting the two imports makes the native path take over. The fix
belongs in the library.

### `espidf.EventQueue`

ESP-IDF events, readable from Python. The default event loop already existed here —
`common-hal/wifi/__init__.c` creates it and registers Wi-Fi handlers — but nothing
of it reached Python, so several things the chip knows were simply thrown away.
`WIFI_EVENT_AP_STACONNECTED` and `AP_STADISCONNECTED` had **empty case bodies**, so
a board running as an access point could not tell that a client had joined; the
disconnect reason was logged and dropped.

```python
q = espidf.EventQueue(espidf.WIFI_EVENT, espidf.ANY_ID, size=16)
while True:
    ev = q.get()                  # None, or (base, event_id, time_us, data)
    if q.overflowed:              # something was dropped
        q.clear()
```

Handlers run in the event loop task, on the other core, alongside the Wi-Fi stack.
Nothing there may touch the interpreter or block, so the handler only copies the
event into a FreeRTOS queue and calls `port_wake_main_task()`; Python drains it
whenever it gets round to it. That is the same shape as the Wi-Fi handler next door
and as `keypad.EventQueue`. **The handler's argument is a static slot, never the
Python object**, so even a stale registration cannot write into the GC heap — the
object is traced as well, for the ordinary reason that the ESP-IDF holds a pointer
to something inside it.

Soft reset unregisters through those same static slots rather than through the
traced list, which is the lesson the `Timer` comment nearby records: the list may
already be gone by then.

**The payload length has to come from a table, and the first version got this
wrong.** `esp_event` does not tell a handler how long `event_data` is, so the
handler copied a fixed 48 bytes — but `esp_event_post_to` stores the payload in
`calloc(1, event_data_size)`, exactly the posted size. For `WIFI_EVENT_SCAN_DONE`,
whose payload is 8 bytes, that read 40 bytes past a heap allocation. The size is a
property of the `(base, id)` pair, so there is now a `sizeof`-driven table and
anything not listed yields empty `data` rather than a guess. Caught by a test that
asked why a scan result claimed to be 48 bytes long.

`common-hal/wifi/__init__.c` — `ESP_ERROR_CHECK(esp_event_loop_create_default())`
became tolerant of `ESP_ERR_INVALID_STATE`. An `EventQueue` may be the first thing
to want the loop, and creating it twice returns that code; `ESP_ERROR_CHECK` would
have aborted the board over an entirely normal condition.

Limits: four queues at once, 1 to 64 events each, payloads truncated at 48 bytes,
and the known bases are `WIFI_EVENT` and `IP_EVENT` — those are the only two the
firmware links, `WIFI_MESH_EVENT` being present but never fired.

Verified with 41 checks: argument validation, the four-queue ceiling and slot reuse
after `deinit`, real events from a scan and an access point start/stop, payload
lengths matching `sizeof` for each event that has one, a decoded `SCAN_DONE`
payload, overflow on a one-slot queue against a 32-slot control, `clear`, double
`deinit`, use after `deinit`, and an id filter passing only its own event. Soft
reset cleanup is covered too, since the suite claims all four slots and passes
again on the next run. Wi-Fi still associates afterwards.

**Not verified:** constructing an `EventQueue` *before* Wi-Fi has ever started. This
board auto-connects at boot because `CIRCUITPY_WIFI_SSID` is set, so the default
loop always exists by the time `code.py` runs, and only the other order — the queue
finding the loop already there — was exercised.

### `espidf.eap_enable()` / `eap_disable()` — WPA2/WPA3 Enterprise

`wifi.AuthMode.ENTERPRISE` already came back from a scan, but `wifi.radio.connect()`
takes only a PSK, so an enterprise network could be seen and never joined. The
credentials go in through `espidf` rather than through `connect()`, because that
binding is shared by every port while this is specific to the ESP-IDF supplicant.
`esp_wifi_sta_enterprise_enable()` is a standing mode, so `connect()` works unchanged.

```python
espidf.eap_enable(identity="anonymous@example.org",     # outer identity
                  username="user@example.org",
                  password="...",
                  ttls_phase2=espidf.TTLS_PHASE2_MSCHAPV2)
wifi.radio.connect("eduroam")                            # no password
```

TLS instead of PEAP/TTLS takes `client_cert` and `client_key`, which must be given
together — `esp_eap_client_set_certificate_and_key` takes both at once, and a
certificate without its key is silently useless.

**The certificates are held by reference, not copied.** `esp_eap_client.c` sets
`g_wpa_ca_cert = ca_cert` and keeps the caller's pointer, while identity, username
and password are `os_zalloc`'d copies. A `bytes` object handed to `ca_cert` and then
collected would leave the supplicant reading freed memory during a handshake that
happens much later, so the objects are held in a traced root pointer until
`eap_disable()` — which tells the supplicant to forget them *before* releasing them,
not after.

An empty string for any of these is rejected here rather than in the IDF, which
returns `ESP_ERR_INVALID_ARG` and surfaces as "Invalid argument" with no hint which
argument was meant.

### `espidf.smartconfig_start()` / `smartconfig_result()` / `smartconfig_stop()`

A phone hands the board an SSID and password over the air, instead of `settings.toml`
being edited.

```python
wifi.radio.start_station()
espidf.smartconfig_start()                  # or sc_type=espidf.SC_TYPE_AIRKISS
while (r := espidf.smartconfig_result()) is None:
    time.sleep(0.2)
ssid, password, bssid = r
wifi.radio.connect(ssid, password)
espidf.smartconfig_stop()                   # only now
```

Three module functions rather than an object, because only one can run at a time.
The result is 114 bytes, more than `EventQueue` carries, so it is kept whole in a
static and read back through `smartconfig_result()`; the other `SC_EVENT` ids have
no payload and can be watched with an `EventQueue` if the progress matters.

**Two things about the order.** `smartconfig_stop()` belongs *after* the connection,
not before: the acknowledgement to the phone is sent once the board has an address,
so stopping earlier makes the phone report a failure even though the board joined.
And **starting SmartConfig drops the current connection** — it has to hop channels to
listen, which breaks the association. That is inherent, not a defect, but it means an
application has to reconnect itself afterwards.

`ports/espressif/Makefile` — added
`esp-idf/components/wpa_supplicant/esp_supplicant/include` to `INC`. `esp_eap_client.h`
lives there and nothing had needed it before.

**Cost: about 70 kB of flash.** This is the first thing in the build to call the
supplicant's EAP code, so all of it linked in at once; 203872 bytes remain free in
the 2 MB application partition.

Verified with 42 checks: every constant present, both `client_cert`/`client_key`
half-pairs rejected, `ttls_phase2` range, empty strings, a wrong type, PEAP and TLS
configurations accepted, `gc.collect()` between `eap_enable` and use (the certificate
retention), `eap_disable` idempotent, `sc_type` range, double `smartconfig_start`
rejected, `stop` idempotent, restart after stop, and ordinary WPA2 still connecting
afterwards.

**Not verified: an actual enterprise association.** That needs a RADIUS server or
eduroam in range, and there is neither here. What is confirmed is that the
configuration is accepted, the mode toggles, and normal WPA2 is unaffected — not that
a login succeeds. SmartConfig is likewise verified only as far as start, stop, event
registration and the connection-dropping behaviour; **no phone was paired**, so the
credential hand-off itself is untested.

### `espidf.pbkdf2()`, `hmac_sha256()`, `aes_gcm_encrypt()`, `aes_gcm_decrypt()`

The crypto the ESP-IDF already had. `aesio` is tiny-AES-c, a portable software
implementation, and the build had no HMAC and no PBKDF2 at all, so both had to be
written in Python — two interpreter round trips per hash iteration, which is what
made a key derivation take seconds. mbedtls is linked here for TLS and
`CONFIG_MBEDTLS_HARDWARE_AES` and `..._SHA` are on, so only the wrappers are new.

**Written against PSA for two of the three.** mbedtls 4, which arrived with ESP-IDF
6.0, moved `gcm.h`, `pkcs5.h` and `aes.h` into `mbedtls/private`. Reaching in there
would mean building on headers upstream has explicitly withdrawn, so the key
derivation goes through `psa_key_derivation_*` with
`PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)` and the AEAD through `psa_aead_encrypt()` /
`psa_aead_decrypt()` with `PSA_ALG_GCM`. `hmac_sha256()` still calls
`mbedtls_md_hmac()`, because `md.h` stayed public.

PBKDF2 wants its inputs in a fixed order — cost, salt, password — and
`psa_aead_encrypt()` appends the tag to the ciphertext itself, which happens to be
exactly the layout `aes_gcm_decrypt()` already expected.

```python
espidf.pbkdf2(password, salt, iterations, key_length=32)   # PBKDF2-HMAC-SHA256
espidf.hmac_sha256(key, data)                              # 32 bytes
espidf.aes_gcm_encrypt(key, nonce, data, associated_data=b"")
espidf.aes_gcm_decrypt(key, nonce, data, associated_data=b"")
```

The GCM tag is carried **inside** the ciphertext rather than returned beside it, so
decryption cannot be asked for without it, and a failed tag raises `ValueError` with
the plaintext zeroed and discarded rather than returned. Forgetting to check the tag
is the usual way authenticated encryption gets misused. `password` and `salt` accept
`str` as well as buffers; `iterations` is capped at ten million so a typo cannot lock
the board up uninterruptibly.

**AES-192 is rejected on this chip.** `SOC_AES_SUPPORT_AES_192` is defined only for
the original ESP32 and the S2, so the S3's AES peripheral has no 192 bit key and
importing one fails. Found by a test vector; without the explicit check it surfaces
as an opaque key import error. The guard is
`#if !defined(SOC_AES_SUPPORT_AES_192)` so the code stays right if it is ever built
for a chip that has it.

The key is imported into the PSA key store for the one call and destroyed before
returning, so nothing of it outlives the operation.

Verified against reference values computed independently — 39 checks, none failing:
RFC 4231 cases 1 and 2 for HMAC, published PBKDF2-HMAC-SHA256 vectors at 1, 2 and
4096 iterations plus key lengths 20, 32, 64 and 1024, and McGrew's GCM test cases
1, 2, 3, 4, 15 and 16 in both directions, covering AES-128 and AES-256, with and
without associated data, and an empty plaintext. The negative side covers wrong
key, wrong nonce, changed associated data, a flipped bit in the body, a flipped bit
in the tag, data too short to hold a tag, AES-192, bad key lengths and an empty
nonce. Nonce lengths of 1, 8, 13, 16 and 32 bytes all round-trip, so PSA has not
narrowed GCM to 12 byte nonces the way some implementations do.

**Measured, and the earlier estimate was wrong twice over.** Predicting 50-80x for
PBKDF2 was too optimistic to begin with; and the move to PSA then gave some of it
back.

| | Python | mbedtls (10.2) | PSA (10.3) |
| --- | ---: | ---: | ---: |
| PBKDF2, 4096 iterations | 4755 ms | 233 ms | **371 ms** |
| PBKDF2, per iteration | — | ~57 us | **~90 us** |
| HMAC-SHA256 of 32 bytes | 1058 us | 191 us | **355 us** |
| GCM over 4 kB | 10575 us | 2074 us | **3787 us** |
| GCM over 32 bytes | — | — | **990 us** |

Still 13x over Python for a key derivation, but 1.6x slower than the same thing on
mbedtls 3. `hmac_sha256()` moved too, and its code did not change at all, so this
is not the PSA wrapper alone: CircuitPython 10.3, mbedtls 4 and ESP-IDF 6.0 all
arrived together and the cause has not been isolated to one of them.

**The hardware is not the fast part here**, which is why none of this is anywhere
near what a 240 MHz core should do to two 64 byte SHA-256 compressions. The cost is
the SHA peripheral being acquired and released around each update, and it shows in
GCM as well: 2.8x over software AES at 4 kB but far less on small buffers, because
the ESP port computes GHASH in software and per-call setup dominates.

So a practical iteration count is around 5000 (0.45 s). OWASP currently suggests
600000 for PBKDF2-HMAC-SHA256, which would be 54 s here and is not reachable.
Whether turning `CONFIG_MBEDTLS_HARDWARE_SHA` off would make small-block hashing
faster is untested — it would also change TLS.

Even at small sizes `aes_gcm_encrypt` is the right choice over composing `aesio`
with a separate MAC: 321 us against 1108 us for the same thing assembled in Python,
because the Python glue — padding, buffer allocation, concatenation — costs more
than the cipher does.

`CRYPTO.md` is the usage side of this: what every crypto module in the build offers,
the `aesio` and `hashlib` limitations worth knowing before you hit them, a verified
recipe for password-protected NVS values, and what that does and does not defend
against on a board with a writable CIRCUITPY drive.

### `espidf.check_heap()`

```python
espidf.check_heap()   # True if every block in the ESP-IDF heap is intact
```

Wraps `heap_caps_check_integrity_all()`. A write past the end of an allocation
shows up here immediately instead of as a hard fault somewhere unrelated later —
which is exactly how the `Palette.c` overflow in section 4 had to be tracked down
the hard way.

### `espidf.ble_prefer_phy()`

```python
espidf.ble_prefer_phy(4)   # 1 = 1M (default), 2 = 2M, 4 = Coded / long range
```

`_bleio` hardcodes the 1M PHY (`Adapter.c:595`) and never tells the user which PHY
a connection uses (`Connection.c:61`). This asks nimble, through
`ble_gap_set_prefered_default_le_phy()`, to prefer another for future
connections. On the S3, Coded PHY is Bluetooth 5 long range: roughly 4x the
distance at an eighth of the rate, useful for reaching beacons that 1M cannot.
Existing connections are unaffected and the peer has to agree.

Built only with `CIRCUITPY_BLEIO_NATIVE`. Port-local: it does not change the
shared `_bleio` API.

### `espidf.wifi_raw_tx()`

```python
import wifi, espidf
wifi.radio.enabled = True
espidf.wifi_raw_tx(frame, channel=1)   # frame is 24..1500 bytes, no FCS
```

Sends a raw 802.11 frame through `esp_wifi_80211_tx()`. The hardware appends the
FCS. WiFi has to be started; `channel` moves there first.

**This is a test and development tool for hardware and networks you own and are
authorised to test.** The radio sends whatever bytes it is given, which is also
what is used to spoof frames and deauthenticate other devices, and is why
CircuitPython does not expose it. It is here because this fork develops a WiFi
monitor and needs to generate traffic to test against. Do not point it at
networks or devices that are not yours.

### `espidf.wifi_sleep_min_active_time()`

```python
import espidf
espidf.wifi_sleep_min_active_time(250)   # milliseconds
```

How long the radio stays awake after a packet before it may sleep again, with
`wifi.PowerManagement.MIN` or `MAX` in effect. Every further packet restarts the
window, so a peer that talks more often than this keeps the radio up and never
waits for the next DTIM beacon.

ESP-IDF sets this once at init from `CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME`,
which menuconfig caps at 60 ms. The underlying
`esp_wifi_set_sleep_min_active_time()` is an ordinary runtime setter in
`esp_private/wifi.h` and takes microseconds; the 8..60 range is menuconfig
validation, not a driver limit. Exposing it turns a two-position switch into a
dial between `MIN` and `NONE`.

What it buys, measured against a client polling every 200 ms with power
management left at `MIN`:

| awake window | p50 |
| --- | ---: |
| 50 ms (the default) | 97.4 ms |
| 150 ms | 98.1 ms |
| 250 ms | **15.3 ms** |

Once the window exceeds the spacing of the requests the radio stops sleeping
between them and latency falls to the network floor -- the same 15 ms that
`wifi.PowerManagement.NONE` gives -- while still sleeping through every longer
gap. Below the spacing it makes no difference at all: the radio sleeps either way.

The rule of thumb for a server answering a regular poll is
`wifi_sleep_min_active_time(poll_interval_ms + 50)`. For irregular traffic there
is nothing to predict from and `NONE` is the only lever left.

Applies to the whole radio, not one socket. See section 5 for what this is for.

`ports/espressif/boards/m5stack_cardputer/sdkconfig` also raises the compile time
default from 50 ms to 60 ms, which is as far as menuconfig allows.

### `espidf.pin_status()`

Read what a GPIO is actually doing, straight from its registers, and whether
CircuitPython has it claimed.

```python
espidf.pin_status(13)
# {'number': 13, 'free': False, 'input': True, 'output': False,
#  'pull_up': True, 'pull_down': False, 'level': 1, 'drive': 2,
#  'function': 1, 'out_signal': 73, 'sleep_mode': False}
```

`free` is CircuitPython's own allocator (`pin_number_is_free()` over the
`_in_use_pin_mask` in `common-hal/microcontroller/Pin.c`). The rest is the
silicon via `gpio_get_io_config()`, regardless of who set it. CircuitPython does
not record *which object* claimed a pin, so this cannot name the culprit, but
`function` and `out_signal` usually make the peripheral obvious — on the cardputer
a boot scan shows GPIO35/36 on out_signals 68/66 (the SPI display), GPIO13/38 on
73 (I2C), and so on.

`level` is the input register, so it only means something when `input` is True: a
pure output has its input buffer off and then it reads 0 whatever the pad drives.
Numbers that exist in range but are not usable GPIOs on this chip (reserved, flash
pins) raise ValueError.

### `espidf.set_vendor_ie()`

Put a vendor-specific information element into the beacons the radio sends as an
AP, so nearby devices see custom data without connecting. The complement of
`wifi.Monitor` on the other side.

```python
wifi.radio.start_ap("MyAP", "password")
espidf.set_vendor_ie(b"hello", oui=b"\x12\x34\x56")   # up to 251 bytes
espidf.set_vendor_ie(None)                             # remove it
```

The element is cleared before each set, because the IDF returns INVALID_ARG when
the same element is set twice and it survives a soft reset.

### `espidf.running_partition()` and a filesystem on a partition

```python
espidf.running_partition()   # -> "ota_0", the label the firmware runs from
```

The one partition writes are refused on, so the one to avoid for data. Detect it
with this, never by trying to erase — an erase to probe destroys a filesystem
that was there.

A partition can hold a real FAT filesystem. `partition_disk` (frozen Python) wraps
`espidf.Partition` as a block device for `storage.VfsFat`:

```python
import espidf, storage
from partition_disk import PartitionDisk

disk = PartitionDisk(espidf.Partition("ota_1"))
fs = storage.VfsFat(disk)
try:
    storage.mount(fs, "/data")          # /data must exist as a dir on CIRCUITPY
except OSError:
    storage.VfsFat.mkfs(disk)           # first time: format
    storage.mount(fs, "/data")

open("/data/big.json", "w").write(...)  # normal file access
```

Verified on the board: format, `open`/`read`/`write`, `mkdir`, `os.listdir`,
`os.statvfs`, and a run counter that survives soft reset (1 → 2 → 3), which means
it survives power loss too — the bytes are in flash. Flash erases in 4096-byte
sectors while FAT writes 512, so a write reads the sector, splices, erases and
writes it back: correct but heavy, and with no wear levelling, so this is for
occasional writes of large read-mostly files (the 66 kB / 172 kB JSON tables that
would not fit on the heap), not a busy filesystem.

Two things worth knowing:

- **It is writable from Python even while the USB drive is mounted.** The root
  CIRCUITPY filesystem is read-only to Python under USB mass storage; a separately
  mounted partition is a different filesystem USB does not expose, so it sidesteps
  that.
- **A Python block device is CircuitPython-only.** USB MSC serves blocks from the
  USB task with no interpreter around, so every LUN must be a *native* (C) block
  device. To get a partition onto the host, see `espidf.expose_partition()` below.

### `espidf.expose_partition()` — a partition as a second USB drive

`ports/espressif/supervisor/partition_disk.c` is a native block device over
`esp_partition`, plus a mass storage LUN, so a partition can be a second drive the
host sees. Everything is static: MSC runs outside the VM and the mount has to
survive an auto-reload.

```python
# boot.py — CircuitPython presents itself to the host after boot.py runs, so this
# is the only place the set of USB drives can be decided.
import espidf, os

try:
    os.stat("/PC.TXT")          # marker on CIRCUITPY picks the direction
    pc_mode = True
except OSError:
    pc_mode = False

try:
    espidf.expose_partition("ota_1", path="/logs", usb_writable=pc_mode)
except OSError:
    espidf.expose_partition("ota_1", path="/logs", usb_writable=pc_mode, format=True)
```

Verified end to end on the board: the partition appears in Windows as a 2 MB FAT
drive, the device appends to `/logs/log.txt` across reboots, and the host reads
those lines and writes its own file back.

`usb_writable` picks the direction, because only one side may write a FAT
filesystem at a time — CircuitPython enforces this to stop two writers corrupting
it. False (default) lets CircuitPython write, for logging, and the host sees it
read-only; True is the reverse, and the device then gets `OSError: [Errno 30]
Read-only filesystem`. `format=True` runs mkfs first, destroying what was there.

All three of the traps in section 0 apply here: `boot.py` runs only on a hard
reset, `usb_connected` is useless inside it, and only one side may write. Detect
the running partition with `espidf.running_partition()`, never by trying to erase.

### `espidf.CSI`

Channel state information: for every received frame the radio reports how the
channel distorted it, which changes when something in the room moves. Where RSSI
is one number per frame, this is one complex value per OFDM subcarrier, and being
frequency resolved it registers a change in the multipath even when the received
power does not move.

```python
import array, wifi, espidf
from ulab import numpy as np

c = espidf.CSI(queue=32, source=wifi.radio.ap_info.bssid)   # verified: same MAC
buf, meta = bytearray(256), array.array("i", [0] * 16)
amp = np.zeros(64, dtype=np.float)

wifi.radio.ping(wifi.radio.ipv4_gateway, timeout=0.1)   # CSI needs traffic
n = c.readinto(buf, meta)              # raw [imag, real] int8 pairs, no allocation
n = c.readinto_amplitude(amp, meta, 4) # magnitudes as floats, ready for numpy
rec = c.packet()                       # convenient dict, allocates
c.queued(); c.lost(); c.deinit()
```

**Frames must be arriving, and `wifi.Monitor` is not the way to get them.**
Measured on ESP-IDF v6.0.1: with a connection and a ping loop, 42 records in
8.3 s, first one within 3 to 21 ms, rssi -40, `sig_mode` 1, 128 bytes. With a
`wifi.Monitor` running, zero records — on the connected channel and on any other,
while the monitor itself was receiving 16 frames in the same window. Promiscuous
mode suppresses CSI rather than feeding it. With no traffic at all, also zero.
The earlier note here claiming a monitor on channel 1 worked was wrong; the test
that was supposed to cover it counted its own empty result as a pass.

The constructor exposes `wifi_csi_config_t`: `lltf`, `htltf`, `stbc_htltf2`,
`ltf_merge`, `channel_filter`, `shift` (`None` for automatic scaling, 0 to 15 to
fix it) and `dump_ack`. On this radio every combination returned 128 bytes for
HT20 and HT40 alike, so the truncation at `ESPIDF_CSI_MAX_BYTES` was not reached;
the knobs are there for hardware where it is.

`source` takes a six byte MAC and drops everything else inside the callback,
before it costs a queue slot. Channel state from two transmitters describes two
different paths, so anything comparing records over time wants it set. Verified
both ways: the AP's own address passes 15 of 15, a fabricated address passes
none and loses none.

Each record carries `rssi`, `noise_floor`, `channel`, `sig_mode`, `rx_state`,
`rx_seq`, `cwb`, `first_word_invalid`, `mac`, `timestamp` and `data`. Three of
those decide whether a record is usable at all: `sig_mode` gives the frame type
and so the layout of `data`, `rx_state` is nonzero for a frame received with
errors, and `first_word_invalid` marks the leading four bytes as not being
measurements.

`readinto(data, meta)` writes the record into caller-supplied buffers and returns
the byte count, `meta` being sixteen 32-bit ints an `array('i')` reads directly.
**Measured: 0 bytes of heap per call, against 336 bytes for `packet()`** over 20
reads each. At a hundred frames a second that is the difference between a
constant 34 kB/s of garbage and none. `readinto_amplitude(out, meta, skip)`
computes `sqrt(im^2 + re^2)` per subcarrier and writes native floats straight
into a `ulab` array, leaving it ready for `numpy` with no conversion; against a
Python reference over 15 frames each the means agreed to 0.1 % (9.13 versus
9.14). The point is not that `sqrt` is slow — the S3 has an FPU — but that sixty
Python level float operations per frame are.

Both validate their buffers before looking at the queue, so a wrong size raises
whether or not a frame happens to be waiting. Validating after the empty check
left the mistake latent until the timing changed, which is how the tests found it.

**Wi-Fi 6 parts describe CSI differently, and the module now handles both.**
Where `SOC_WIFI_HE_SUPPORT` is set — C5, C6, C61 — the IDF redefines two of the
structures this code touches, so what was written for the S3 did not compile
there at all:

| | pre-Wi-Fi-6 (ESP32, S2, S3, C3, C2) | Wi-Fi 6 (C5, C6, C61) |
| --- | --- | --- |
| `wifi_csi_config_t` | `lltf_en`, `htltf_en`, `stbc_htltf2_en`, `ltf_merge_en`, `channel_filter_en`, `manu_scale`, `shift` | `wifi_csi_acquire_config_t`: `acquire_csi_legacy`, `acquire_csi_ht20/ht40/su/mu/dcm/beamformed`, `val_scale_cfg` |
| frame format | `rx_ctrl.sig_mode`, 0/1/3 | `rx_ctrl.cur_bb_format`, 0=11b 1=11g 2=HT 3=VHT 4=HE&nbsp;SU 5=HE&nbsp;MU 6=HE&nbsp;ER&nbsp;SU 7=HE&nbsp;TB 11=VHT&nbsp;MU |
| bandwidth | `rx_ctrl.cwb` | no such field; derived from `rx_ctrl.second` being nonzero |
| scaling range | 0 to 15 | 0 to 8 on MAC version 3 (C5, C61), 0 to 3 on version 2 (C6) |

Only `dump_ack_en` survives unchanged. The Python arguments are the same on every
part so a script ports without editing, and four Wi-Fi 6 only arguments were
added — `he_su`, `he_mu`, `he_dcm`, `he_beamformed` — for the HE-LTF acquisition
that has no pre-Wi-Fi-6 counterpart. `stbc_htltf2`, `ltf_merge` and
`channel_filter` have no Wi-Fi 6 counterpart and are ignored there. `sig_mode` is
the one place where portable code has to know which part it is on: the two scales
disagree, 1 meaning HT on one and 11g on the other.

The STBC selector is deliberately left at its default, because it is called
`acquire_csi_he_stbc` on MAC version 2 and `acquire_csi_he_stbc_mode` on version
3 and zero means the same on both — not naming it avoids a third branch.
`ESPIDF_CSI_MAX_BYTES` defaults to 512 rather than 128 on Wi-Fi 6 parts, since an
HE20 record carries 242 tones against 64; that figure is sized from the tone
count and has not been checked against hardware.

Verified by building `espressif_esp32c6_devkitc_1_n8` with
`CIRCUITPY_ESPIDF_CSI=1`: the module compiles and links, with
`esp_wifi_set_csi`, `esp_wifi_set_csi_config`, `esp_wifi_set_csi_rx_cb` and the
whole of `espidf_csi_*` present in the ELF. That build then fails its size check,
because the C6 image was already over its 2 MB app partition before this module
was added — nothing to do with the change. The Wi-Fi 6 path has not been run on
hardware; there is no such board here yet.

The macros are tested with `defined()` rather than bare, because the port builds
with `-Wundef -Werror` and `CONFIG_SOC_WIFI_HE_SUPPORT` is simply absent on parts
without it.

Needs `CONFIG_ESP_WIFI_CSI_ENABLED=y`, added to the board sdkconfig, and the
`CIRCUITPY_ESPIDF_CSI` build flag. Only one instance at a time. The IDF callback
runs in the WiFi task, which is pinned to core 0 while the VM runs on core 1, and
its buffer only lives for the duration of the call, so records are copied into a
ring buffer outside the heap that Python drains; when it is full new records are
dropped rather than blocking the radio. Each record keeps at most
`ESPIDF_CSI_MAX_BYTES` (128) of channel data.

### `espidf` module additions

| Function | Returns |
| --- | --- |
| `get_time_us()` | Microseconds since boot, one microsecond resolution |
| `get_cycle_count()` | Raw CPU cycle counter, wraps every 17.9 s at 240 MHz |
| `task_stats()` | FreeRTOS run time statistics per task |
| `prof_stats()`, `prof_reset()` | Cycle probes, needs `CIRCUITPY_PROF=1` |
| `profiler_start()`, `profiler_stop()`, `profiler_data()` | PC sampling profiler |
| `bench_setjmp()`, `bench_div()` | Micro-benchmarks used while measuring |

### Profiler

`supervisor/prof.h` and `ports/espressif/supervisor/prof.c`, built only with
`make CIRCUITPY_PROF=1`. Cycle-counting probes plus a sampling profiler that reads
the program counter of the CircuitPython task from a separate task and reports a
histogram. `reset_port()` stops the sampler, without which a run started before an
auto reload keeps sampling through the reparse of the new code.

The probes sit in the display path and cost about **1.5 ms per frame**, so the
firmware you actually run should be built without `CIRCUITPY_PROF`.

Probe points, all in `shared-module/busdisplay/BusDisplay.c` except where noted:

| Probe | Covers |
| --- | --- |
| `refresh_area` | one whole dirty area |
| `get_areas` | walking the group for dirty areas, once per refresh |
| `area_setup` | clipping and the subrectangle arithmetic before the loop |
| `set_region` | the column and row window commands |
| `fill_area` | composing one subrectangle |
| `chunk_bus` | per subrectangle: waiting out the previous transfer, then closing and reopening the bus transaction |
| `send_pixels` | handing one subrectangle to the bus |
| `gc_collect` | one garbage collection |

`chunk_bus` is worth reading carefully: it is mostly the DMA wait, not overhead,
so it being large is not automatically a problem to fix.

---

## 3b. `registers` module and `register_map`

Two layers for talking to register-based I2C/SPI devices faster than the pure
Python `adafruit_register` does. The C module is portable (no ESP dependency) and
built for any full build; the Python class is frozen into this board's firmware.

### `registers` (C, `shared-bindings/registers/`, `shared-module/registers/`)

The bit twiddling every register driver does, in C. Two functions over a buffer
holding one register (1 to 8 bytes), no I/O:

```python
import registers
registers.extract(buffer, mask, shift, signed=False, lsb_first=True)  # -> int
registers.insert(buffer, mask, shift, value, lsb_first=True)          # in place
```

`mask` selects the field and already sits at its position; `shift` moves it down
to a value. Signed fields are two's complement. Wide registers return a full int,
so a 32-bit value does not overflow. Positional args, not a keyword map, because
the keyword parsing was measured to cost more than the work: `extract` on a 2-byte
register is 8.5 µs against 13.4 µs for the same in Python, and the gap widens with
register width. Gated by `CIRCUITPY_REGISTERS`.

### `register_map` (frozen Python, `frozen/register_map/`)

A named register map on top of `registers`. The goal is a short, readable I2C (or
SPI) sensor driver: describe the fields once, then read and write them as
attributes.

```python
from register_map import RegisterMap
imu = RegisterMap.from_i2c(i2c_device, {
    "whoami":     (0x75, 0, 8, {"mode": "r"}),
    "reset":      (0x6B, 7, 1),
    "clock":      (0x6B, 0, 3, {"values": {"internal": 0, "pll_x": 1, "stop": 7}}),
    "gyro_range": (0x1B, 3, 2, {"values": {"250dps": 0, "500dps": 1,
                                           "1000dps": 2, "2000dps": 3}}),
    "temp":       (0x41, 0, 16, {"signed": True, "width": 2, "lsb_first": False,
                                 "scale": 1 / 340, "offset": 36.53, "mode": "r"}),
})

imu.reset = 1
imu.gyro_range = "500dps"    # a name, not a magic number
imu.gyro_range               # -> "500dps"
imu.temp                     # -> 23.7  (already in degrees C)
imu["gyro_range"] = 3        # [name] access works too
```

Three things make the definition read like a datasheet and the use read like
plain Python:

- **Attribute access** — `imu.gyro_range = x`. Needs `__setattr__`, which this
  build now has (see below).
- **Named values** — `"values": {name: int}`. Read returns the name (or the raw
  int for an undeclared combination), write takes a name or an int.
- **Scale / offset** — `"scale"`, `"offset"`. Read returns `raw * scale + offset`
  in physical units, write inverts it. `"mode": "r"` marks a field read-only.

Plus **opt-in per-register caching**, which is the point of the cache being
opt-in: a shadow of a register the device changes under you goes stale.

Two cache modes per register:

- `"none"` (default): every read hits the bus, every write is read-modify-write.
  Correct for anything the device changes — status, data, self-clearing bits.
- `"full"`: the shadow is authoritative. Reads come from it, writes update it and
  the device with no read-back. Only for registers the host alone writes.

`with m.batch():` defers writes to cached registers and flushes each touched
register once at the end. Measured against a transaction-counting fake bus: three
fields across two registers is **2 bus writes** in a batch, against 6 transactions
(3× read-modify-write) the naive way.

The staleness escape hatch is explicit: `m.sync("field")` re-reads one cached
register from the device, `m.sync_all()` re-reads all of them. Use after a device
reset or when something else touched a cached register. The default being "none"
means nothing is cached unless you declare you own it, so the incoherence can only
happen where you asked for it.

Also over SPI: `RegisterMap.from_spi(spi_device, fields, ...)`, same accessor idea
as `adafruit_register.register_accessor` (R/W bit is the top bit of the address).

### `MICROPY_PY_DELATTR_SETATTR` enabled (`ports/espressif/mpconfigport.h`)

`__setattr__` and `__delattr__` on user classes, off by default at this ROM level.
Turned on because CPython has it, drivers assume it, and it is what lets a register
map intercept `imu.gyro_range = x`. Only classes that actually define these methods
pay for it — they lose the attribute-store fast path from section 1; a plain class
still stores an attribute in 124 cycles, unchanged. This changes behaviour: a
`__setattr__` that used to be silently ignored now runs.

## 4. Bugs found

### In this fork, during testing

`del x[i]` is compiled as a store of `MP_OBJ_NULL`, which `mp_obj_subscr()` reads
as a delete. The store fast paths took it as a value, so `del lst[0]` wrote `NULL`
into the list instead of removing the element — a crash waiting for the next read
of that slot. It survived seven test suites before a dedicated deletion test found
it. The whole fast-path block in `STORE_SUBSCR` is now under `value != MP_OBJ_NULL`.

**The same bug was in `STORE_ATTR`, and there it rebooted the board.** The guard
above was written for subscripts in the very commit that added both shortcuts, and
the attribute sibling twelve lines up did not get it. `del obj.attr` wrote
`MP_OBJ_NULL` into the instance members map as an ordinary value; the `LOAD_ATTR`
fast path tests whether the *slot* was found rather than what is in it, so the next
read of that name handed `MP_OBJ_NULL` to the interpreter. On the unix port that
prints as `(nil)`; on the board the firmware reboots.

`getattr()` and `hasattr()` were correct throughout, because they take the general
path — that asymmetry is what identified the fast path as the culprit.

Only this fork is affected. The `LOAD_ATTR` shortcut is upstream MicroPython
(`7b89ad8dbf`, 2021) and is harmless on its own; nothing there ever writes a null
value into the map. The `STORE_ATTR` shortcut exists only here, and `main`,
`main-base` and `adafruit/main` contain no trace of it — upstream sends the delete
to `mp_obj_instance_store_attr()`, which removes the entry with
`MP_MAP_LOOKUP_REMOVE_IF_FOUND`.

The guard costs about **6 cycles on every instance attribute write** (124.5 →
roughly 130, measured three times); reads and everything else are unchanged. Put
on the store rather than the load deliberately: writes are less frequent than
reads, and a check on the load would tax the more common path.

Found by running MicroPython's own test suite against the unix port, which builds
the same `py/` this firmware does. `basics/del_attr.py` and
`basics/class_descriptor.py` both failed on it; both pass now. Three static
analysers had found nothing here, which is the expected shape — the code is well
formed, it is a missing semantic guard. The suite is in the tree at `tests/` and
this fork does not appear to run it.

The first version of `espidf.Timer` cleaned up on soft reset by walking a list on
the heap, which is already invalid at that point. Timers kept firing into freed
memory after a reload. The handles are now kept outside the heap as well.

`espidf.Timer` created the ESP-IDF timer before claiming a tracking slot, so
exceeding the limit left a timer behind that nothing referred to and that could
not be stopped. The slot is now reserved first.

### Upstream

`py/gc.c` twice, both fixed here and both described in section 1: the
stack-overflow rescan is not bounded by `gc_last_used_block` the way the sweep
is, and `gc_alloc()` stores an allocation-table byte index divided by
`BLOCKS_PER_ATB` when marking an area full, which is the conversion the
`found` path needs and this one does not.

`shared-bindings/digitalio/DigitalInOut.c` — **subclassing `DigitalInOut` and
reading `.value` rebooted the board.** All twenty bindings, the Python methods and
the protocol wrappers alike, did `MP_OBJ_TO_PTR(self_in)` directly; on a subclass
that pointer is the instance object, not the native one in its `subobj`, so
`self->pin->number` came out of unrelated memory. `displayio` avoids this with
`native_group()` → `mp_obj_cast_to_native_base()`; `digitalio` had no equivalent.

Fixed with a `native_digitalinout()` helper on the same pattern, but with the
exact-type test done inline before the call: `mp_obj_cast_to_native_base()` is in
another translation unit and without LTO would put an out-of-line call on every
pin read and write. Measured against a build with the change backed out, the
direct path does not move — store 2640 against 2625 ns, read 2625 against 2594,
`.direction` 2640 against 2594, all inside run-to-run spread. Costs 496 bytes of
flash for the inline checks.

**Correction, 2026-08-30.** That conversion reached eighteen of the twenty entry
points, not all of them. `switch_to_output` and `switch_to_input` were left
casting `pos_args[0]` directly, and both still rebooted the board on a subclass;
`check_for_deinit` did not catch it because the overlaid word reads as `0x1`
rather than NULL. Found by the audit in section 10 and traced on the board —
`.direction` and `.value` work on the very object that `switch_to_output` kills.
The paragraph above described the intent; this is what shipped until now.

`ports/espressif/common-hal/digitalio/DigitalInOut.c` — **`get_pull()` never
returned `PULL_NONE`.** It read `config.pu`, ignored it, and returned
`config.pd ? PULL_DOWN : PULL_UP`, so a pin with no pull at all — which is what
`construct()` leaves and what `switch_to_input(PULL_NONE)` sets — reported
`PULL_UP`. `PULL_NONE` came back only when the HAL call itself failed. Fixed;
`digitalio_test.py` covers the three states and the round trip.

`shared-module/audiodelays/GranularPitchShift.c` — `memset(word_buffer, 32768, ...)`
fills an unsigned 16-bit buffer with silence, except that `memset` repeats a single
byte and 32768 truncates to 0, so it wrote the minimum sample value — full negative
deflection — instead of the midpoint. The 8-bit branch beside it is correct because
128 fits in a byte. Replaced with a loop writing `0x8000`, which is the same
midpoint the mixing code below produces with `word_buffer[i] ^= 0x8000`.
**Untested on hardware:** `CIRCUITPY_AUDIODELAYS` is 0 for this board, so the file
is not in the firmware. It does compile as part of the unix port.

`shared-module/displayio/Palette.c` — `palette_index > color_count` should be
`>=`. Out-of-range read and write past the end of the palette.

`py/asmxtensa.c:37` — `#if N_XTENSAWIN` picks the temporary register, but the
macro is defined by `emitnxtensawin.c`, which includes `emitnative.c`, not this
file. `asmxtensa.c` is its own translation unit, so the macro is never defined
there and the plain temporary A6 is used even for the windowed ABI, against what
the comment on the two registers in `asmxtensa.h:309` describes. Now decided from
the configuration.

`py/asmxtensa.c/.h` — a missing prototype for `asm_xtensa_l32i_optimised()` and
two casts that trip `-Wcast-align`. Only visible with CircuitPython's stricter
warnings.

`shared-module/bitmaptools/__init__.c` — `bitmaptools.dither()` into a 1 bpp
bitmap was broken twice over, and each fault hid the other.

The storing loop ran `bitmap->width` times while consuming 32 booleans and
writing one word per pass, so it wrote 32 times the length of the row: 960 bytes
into a 32 byte row for a 240 pixel image, shredding the rest of the bitmap and
running off the end of the allocation on the last row. On the board a 64x8
dither, whose destination is 64 bytes, wrote about 312 and took out a module
global — `NameError` for a variable assigned six lines earlier. The loop now runs
`(width + 31) / 32` times, and the padding booleans past `width` are cleared once
before the row loop instead of coming from whatever was on the stack.

With that fixed the output was still wrong. `displayio` addresses a 1 bpp pixel
as byte `x >> 3`, bit `7 - (x & 7)`, so the first eight pixels of a row live in
the lowest addressed byte; building a `uint32_t` with pixel 0 at bit 31 and
storing it puts them in the highest one on a little endian target. A white run at
x 0..7 came out at x 24..31, and the tail of a row whose width is not a multiple
of 32 disappeared entirely. The word is now byte swapped on store.

`shared-module/gifio/GifWriter.c` — the frame buffer was `nblocks * 128 + 4`,
which covers neither the 22 bytes a frame writes around its block data (19 byte
header, 3 byte end code) nor the 416 byte file header the constructor builds
before its only flush. `write_data()` merely asserts the bound and the assert is
compiled out. Frames overran whenever `width * height` was a multiple of 126, and
any image under 379 pixels — a 16x16 sprite included — overran inside the
constructor. A 63x2 writer corrupts the object allocated after it and takes out
module globals. Now `MAX(416, nblocks * 128 + 22)`.

`shared-module/tilepalettemapper/TilePaletteMapper.c` — the tile indices are
bounds checked per pixel but the colour index is not, and it comes straight from
the bitmap, limited only by its bits per value. A bitmap with more values than
`input_color_count` read past the end of the mapping row on every pixel. This is
a read, so the damage is arbitrary colours rather than corruption. Out of range
indices are now transparent, matching `displayio_palette_get_color()`.

`shared-bindings/tilepalettemapper/TilePaletteMapper.c` — `input_color_count` was
unvalidated. Zero makes `m_malloc(0)` return `NULL` for every mapping row and the
per-pixel lookup then dereferences it, panicking on the first refresh. Now
required to be at least 1.

`ports/espressif/common-hal/busio/SPI.c` — the result of `spi_device_queue_trans()`
was discarded while the drain loop still waited for `cur_trans` results. One
refusal — most plausibly `ESP_ERR_NO_MEM` when the driver needs a DMA bounce
buffer and the internal heap is under pressure from WiFi or BLE — blocked the
board forever in `spi_device_get_trans_result()` with the bus mutex held: no
traceback, no Ctrl-C, reset only. The transactions that made it into the queue
are now counted and exactly that many drained, because leaving one behind would
break the polling path of the next transfer, and the failure is then reported so
the caller raises `OSError(EIO)`.

### Second batch

Twelve more, from the module audit written up in `AUDIT-FINDINGS.md`. Every one of
them was put through an independent pass that tried to break the fix before it was
flashed; four came back with something real, and those corrections are folded in
below.

`shared-module/lvfontio/OnDiskFont.c` — `self->max_glyphs` was assigned from the
constructor's argument, but the glyph cache is sized by that value capped to what
the font's header census reports. The supervisor asks for
`width_in_tiles * height_in_tiles`, about 240 here, so any font with fewer glyphs
left every scan of `codepoints[]` and `reference_counts[]` running off the end of
its allocation and `load_glyph_bitmap` writing past them.

Two more in the same constructor, the second only found while reviewing the first.
`bitmap->base.type` was stored before the null check, so a failed allocation wrote
through a null pointer. And `allocate_memory()` runs `deinit()` before returning
NULL, while the constructor cleared only `cmap_ranges` — the supervisor builds its
font in a stack local that `port_malloc` does not zero, so a failed allocation
deinited through indeterminate pointers long before any caller could check.
`bitmap`, `codepoints` and `reference_counts` are cleared up front now.

`shared-module/terminalio/Terminal.c` — the escape parser looked ahead as far as
`i[11]` with no regard for the end of the buffer, and then advanced `i` by the
length the sequence would have had. `mp_stream_rw` subtracts the return value from
an unsigned remaining count, so returning more than `len` wrapped it and the write
loop never terminated: a hang, with heap contents rendered onto the display on the
way, reachable from one line of Python. Lookahead goes through a `TERM_PEEK` that
yields 0 past the end, and every advance saturates there. `utf8_get_char` still
takes no length; that needs a cursor the decoder and the parser share, and is
noted in the file rather than fixed.

`shared-module/displayio/Group.c` — `displayio_group_get_previous_area` had no
final `continue` in its else branch, so a member that is neither a TileGrid nor a
Group fell through with `layer_area` never written and an indeterminate stack
struct was copied into the caller's area. In practice that means a `vectorio`
shape inside a nested group: six lines of Python, then `parent.pop()`. It now
reads the shape's area through the draw protocol, the same way `_remove_layer`
already did, and refuses to fall through for anything else.

`ports/espressif/common-hal/wifi/Radio.c` — two out-of-bounds stores in
`connect()`. `password[password_len] = 0` with a validated length of up to 64 into
a 64 byte field wrote the low byte of `scan_method`; `bssid[bssid_len] = 0` with a
six byte MAC into a six byte field wrote `channel`, which the previous line had
just set. Passing both `bssid` and `channel` therefore lost the channel silently
and the driver swept from channel 1 instead of starting on the known one.

`shared-module/epaperdisplay/EPaperDisplay.c` — `_clean_area` tested the bus lock
inverted. It returned on a *successful* acquire, so the whole clean pass was dead
code and an ACeP panel was never pre-cleared; and on a genuine failure it fell
through, drove the bus without the mutex, and then released a lock it did not
hold.

`shared-bindings/displayio/TileGrid.c` — `width` and `height` went into `uint16_t`
fields unvalidated, and subscripting divides by `width`, so a zero grid panicked
the board on an integer divide by zero before the index bounds check could raise.
Range-checked rather than minimum-checked, because the argument is an `mp_int_t`
and 65536 truncates to zero as surely as 0 does. The review found the first
attempt incomplete: `displayio.Bitmap` explicitly allows a zero width or height
and an omitted tile size defaults to the bitmap's, so `bitmap_width % tile_width`
was still `0 % 0`. The tile dimensions are validated before the modulo now.

`shared-bindings/tilepalettemapper/TilePaletteMapper.c` — `width_in_tiles` is only
assigned when the mapper is bound to a TileGrid, so subscripting a freshly
constructed one divided by zero. Two lines of Python panicked the board.

`shared-module/synthio/__init__.c` — the waveform wrap used `>` where `lim` is the
length shifted, so `accum == lim` indexed one `int16` past the end. The review
showed that fixing only the comparison was not enough: `accum` walks
`[offset, lim)`, so the span one subtraction can wrap is `lim - offset`, not
`lim`. Guarding against `lim / 2` was correct only for a note with no loop; with
`waveform_loop_start` above zero a rate between `lim - offset` and `lim / 2`
passed the guard, `accum` grew without bound, and `idx` — an `int16_t` — went
negative and read up to 64 kB below the buffer. The guard now uses the loop span,
the renormalisation folds into the loop region instead of `accum % lim + offset`
which could land at or past `lim` again, and the ring modulator's guard no longer
tests the ring rate against the *main* waveform's limit.

`ports/espressif/module/cardputer_keyboard.c` — `key_to_char` compared `key >`
rather than `>=` the keymap length, so index 56 read one past the end of two
adjacent const arrays, against the contract its own docstring states.

`ports/espressif/common-hal/_bleio/Characteristic.c` — the client role required
`notify_rx.indication == 0` and threw every indication away. NimBLE sends the ATT
confirmation itself before this runs, so the peer sees a healthy link and keeps
sending while the data goes nowhere: no error, no stall, nothing to notice.
Indicate-only peripherals — CTS, battery and health services, several vendor UART
clones — simply did not work.

`ports/espressif/common-hal/busio/SPI.c`, `.h` and
`shared-module/fourwire/FourWire.c` — `set_spi_config` stored the *achieved*
clock in `self->baudrate`, which is what `configure()` compares the caller's
*requested* rate against, so any rate that does not come back unchanged never
matched and the device was torn down and re-added on every single transaction.
With `MAX_SPI_TRANSACTIONS` raised to 20 in this fork that is two FreeRTOS queues
of 20 plus a device allocation each time. The board's own 80 MHz display was not
affected; 24, 30, 12 and 15 MHz are. The two rates are now separate fields and the
`frequency` property still reports the achieved one.

The review found the surrounding failure path worse than the finding described,
and it interacts with the fix: `configure()` removes the old device *before*
calling `set_spi_config`, which used to raise on failure. ESP-IDF does not write
the handle on failure, so `spi_handle[host_id]` kept the freed pointer, and the
raise longjmped out of `FourWire.begin_transaction` with the bus mutex held for
the rest of the session. Worse, with the requested rate now remembered, the next
`configure()` with the same request would match and transmit through the freed
device. `set_spi_config` returns `esp_err_t` now and clears both the handle and
the stored rate on failure; `construct` raises, `configure` returns false, and
`FourWire.begin_transaction` releases the lock and reports failure the same way it
already did for a chip-select error.

`shared-module/bitmapfilter/__init__.c` — none of the six filters marked the
bitmap they write, so a filter applied to a bitmap already on screen showed
nothing from the second frame on. The first frame works because `construct`
initialises the dirty area to the whole bitmap, which is why this survives
one-shot test scripts.

Verified on the board: the unfixed firmware hard faults into safe mode on the test
that covers these; the fixed one passes 35 checks with none failing, the earlier
32-check regression suite still passes, and the TileGrid benchmark is unchanged
within 0.06%.

### Third batch

The remaining correctness findings, plus one defect found only by testing. Each fix
was designed against the code before being written and then handed to an
independent pass that tried to break it; that pass returned four regressions and
four incomplete fixes, all of which are folded in below.

`ports/espressif/common-hal/pwmio/PWMOut.c`, `.h` — `PWMOut.frequency` stored the
new duty resolution and called `ledc_set_freq` without reconfiguring the timer, so
crossing the boundary at 9765 Hz (where a 13-bit timer's maximum frequency and the
13-to-12 bit step coincide exactly) left the duty scaling, the cached
`ledc_timer_config_t` and the hardware disagreeing — and reported success either
way. Downward is worse: construct at 20 kHz then set 1000 Hz and `set_duty_cycle`
writes a 13-bit value into an 11-bit timer, which `ledc_set_duty` does not range
check. The requested 16-bit duty is now kept so it can be re-applied as a fraction
after a resolution change, both LEDC calls are checked, and `self` is only mutated
once one has succeeded.

Review correction: reconfiguring only when the resolution changes was not enough.
`ledc_set_freq` re-reads the timer's concrete clock source from hardware and can
never re-select another one, so a frequency reachable only on a different LEDC
clock started raising `ValueError` where it used to be a silent no-op — even
though the `ledc_timer_config` call the fix had just added would have succeeded.
It now falls back to a full reconfigure whenever `ledc_set_freq` refuses, and
raises only when both fail. `calculate_duty_cycle` also gained a guard: above
80 MHz the interval is 0 and `i - 1` underflowed to `0xFFFFFFFF`, which the clamp
turned into a plausible 13.

`ports/espressif/common-hal/socketpool/Socket.c`, `shared-bindings/socketpool/Socket.c`
— every socket is `O_NONBLOCK` at the lwIP level regardless of the Python timeout,
so `send()` and `sendall()` returned EAGAIN on a blocking socket. Any multi-kB
upload hit it at an arbitrary offset and `sendall` cannot report how much went out,
so there was no way to resume.

Review correction, and the reason this nearly shipped broken: the first version
returned `ETIMEDOUT` once a finite timeout elapsed, which is CPython's semantics
but not this tree's. Every retry loop in the frozen libraries keys off EAGAIN
alone — `adafruit_httpserver`'s `_send_bytes` sets a 1 s timeout on each connection
and re-raises anything else, so a client that stopped reading would have killed the
server loop. Only a socket with no timeout at all waits now; a finite or zero
timeout keeps returning EAGAIN exactly as before. The interrupt path returns
`EINTR` rather than EAGAIN, because two loops in the web workflow spin on EAGAIN
and a pending exception does not clear by retrying.

`socketpool_socket_accept` never carried `timeout_ms` over, so an accepted socket
behaved as non-blocking however the listener was configured. The first fix only
covered the path that allocates a new object; it is now done where both paths pass
through.

`shared-module/_bleio/ScanResults.c`, `py/ringbuf.c`, `py/ringbuf.h`,
`ports/espressif/common-hal/microcontroller/__init__.c` — the audit placed this
allocation in `append()`, the producer. It is in `common_hal_bleio_scanresults_next()`,
the consumer, and `append()` cannot allocate or raise inside its critical section
at all. So no producer-side staging buffer was needed: the consumer now peeks the
queued packet's length under the lock, allocates outside it, and only then takes
the lock again to consume. A `MemoryError` therefore leaves the packet queued, the
buffer consistent and the lock free, where before it skipped the enable and the
interrupt watchdog reset the board within 300 ms with the Python-level error never
delivered. This needed a `ringbuf_peek_n()`, which is additive — nothing else calls
it. `common_hal_mcu_enable_interrupts` now reboots into safe mode on an unbalanced
enable instead of only asserting, matching what atmel-samd and stm already do: the
assert compiles out in release, and a wrapped counter silently turned every later
critical section in the port into a no-op.

`shared-module/lvfontio/OnDiskFont.c`, `shared-module/terminalio/Terminal.c` — the
glyph cache. The refcount was released even when the tile was not replaced, the
full-width pair search needed two adjacent never-used slots and neither wrapped nor
honoured the codepoint offset, and the slot census abandoned itself at the third
distinct advance width. Since `max_glyphs` is capped to what the census reports,
that last one left the terminal blanking every cell that no longer fit.

Review correction: `load_glyph_bitmap` claims its slot before reading pixels and
can then fail, so `cache_glyph` returned -1 with a reference already taken. The new
reacquire path would then push that slot to two references for a single cell and
pin it forever. The failure path now undoes its own claim. The census loop is also
bounded independently of `max_cid`, which comes from the file and underflowed the
old bound to `SIZE_MAX` — and the `FR_OK` checks do not bound it, because
`read_bits` returns `FR_OK` without touching the file when asked for zero bits.

**Found by testing, not in the audit:** `glyph_advance_bits == 0` does not mean
every glyph has zero advance. It means the advance is not stored per glyph because
they all share `default_advance_width` — which is exactly what `lv_font_conv`
emits for a monospace font, the obvious choice for a terminal. `read_bits` answers
0 without reading, so the census discarded every glyph as zero-advance and reported
one usable slot: a 64-slot request produced a 9x16 bitmap and the whole terminal
shared a single glyph. With the earlier `self->max_glyphs` fix in place that is a
visibly broken terminal; without it, it was heap corruption. Now the default
applies when the field is absent.

`shared-module/audiomp3/MP3Decoder.c` — the ID3v2 skip passed offset and whence
swapped and tested the result backwards. Review correction: testing the corrected
return for `>= 0` is not enough either. `stream_lseek` hands back `seek_s.offset`,
which an ioctl that accepts `MP_STREAM_SEEK` without honouring it leaves as the
value passed in; the old swapped call happened to pass `SEEK_CUR == 1` there, so
such a stream failed the `== 0` test and correctly fell through to read-and-discard.
It now compares positions, so only a seek that actually moved counts.

`shared-module/vectorio/Polygon.c` — two integer divisions per edge per pixel
existed only so the last iteration could wrap to the closing edge. `len` is set as
`2 * point_count` and the binding requires at least three points, so `i` is at most
`len + 1` and strictly below `2 * len`: subtracting once is the same as the
remainder. Worth about 1.2x on a triangle, per the calibration in
`AUDIT-FINDINGS.md`, not the 1.5-2x originally claimed.

Verified on the board across six suites, 106 checks, none failing: the three MP3
variants (no tag, a 310 byte tag that fits the input buffer, a 4009 byte tag that
does not) decode to identical sample counts and levels; PWM crosses 9765 Hz in both
directions preserving duty including 0 and 0xFFFF, rejects 100 MHz and stays usable
after; a blocking socket sends 96 kB to a deliberately slow reader with no EAGAIN
while finite-timeout and non-blocking sockets still get it; a monospace lvfontbin
caches 32 distinct glyphs through eight rounds of recycling; `Polygon` matches a
reference implementation over 12696 points; and the two earlier suites still pass.

### Fourth batch

Everything left that was a defect rather than a missed optimisation.

`shared-module/audiomp3/MP3Decoder.c`, `ports/espressif/common-hal/audiobusio/__init__.c`
— **`loop=True` never restarted an MP3.** The first diagnosis was wrong and worth
recording: the audio driver does reset the sample on `GET_BUFFER_DONE`, but it then
fell through to a `sample_buffer_length == 0` test that killed the playback it had
just rewound. Fixing that alone changed nothing, so the driver was instrumented —
`reset_buffer` turned out to be called exactly once, at `play()`, and never again.
The real cause is in the decoder: running out of input at the end of the stream was
reported as `GET_BUFFER_ERROR`, and every backend stops dead on ERROR without
consulting its loop flag. An underflow with `eof` already set is a normal ending and
returns `GET_BUFFER_DONE` now; only a frame that failed to decode for another reason
is an error. What settled it was a control: `audiocore.RawSample` loops forever on
the same driver, so the loop machinery was never the problem. Both fixes are needed —
the driver still has to re-fetch after rewinding.

`ports/espressif/common-hal/busio/SPI.c` — the chunking loop in
`common_hal_busio_spi_transfer` is `while (bits_remaining && !mp_hal_is_interrupted())`
and then returns `true` unconditionally, so an interrupted transfer was a short write
reported as a complete one. `mp_hal_is_interrupted()` is true for *any* pending
exception, and `reload_initiate()` posts one the moment a file lands on CIRCUITPY, so
a routine autoreload during a transfer hits it. Neither caller that matters looks at
the result — `FourWire.send()` is `void` and `sdcardio` discards it. With this fork's
`WRITE_MEMORY_CONTINUE` the dropped bytes are not a hole in the frame: they displace
the controller's address counter, so everything sent afterwards lands at the wrong
offset. The command writes take the polling path and are never dropped, so the
command stream stays intact while the pixel stream shifts. The escape is gone.

`shared-module/audiomixer/Mixer.c` — of the six mono-into-stereo loops, five are
bounded at `i + 1 < n` and one was not, storing `word_buffer[n]` one 32-bit word past
the mix buffer whenever the word count is odd. `buffer_size` is unvalidated and the
count is `floor(buffer_size / 8)`, so 1000, 1032, 24 and 40 all trip it while the
1024 default does not. On this board the stray word lands in the GC block's round-up
padding rather than a neighbouring object, so it is safe by allocator arithmetic
rather than by design. The pair loop is bounded and the odd tail sample written
separately, which keeps the output byte-identical.

`shared-module/lvfontio/OnDiskFont.c`, `shared-module/terminalio/Terminal.c` — two
more in the glyph cache. The cell-width inference keeps two advance buckets that latch
onto the first two distinct advances in glyph order and then halves the larger, which
only means anything for a font with exactly two advances in a 2:1 ratio. For Arial it
latches on the space and the exclamation mark and yields 3 px, the number measured on
the board; nearly every glyph then counts as full width. And `terminalio` released
only the cell under the cursor before caching, so a full-width glyph needing two
adjacent free slots could never be satisfied on a saturated cache.

Verified across six suites, 109 checks, none failing. The MP3 loop now runs 6 s
unbroken on all three tag variants where it used to stop at 2.0 s; a mixer canary
stays intact at buffer sizes 1000, 1024, 1032, 24 and 40; and the four earlier suites
still pass. The display benchmark is unchanged at a 7.75 ms minimum.

### Fifth batch — from an independent source audit

A separate review of `py`, `shared-bindings` and `shared-module` produced 71
source-confirmed findings (`CODE_AUDIT_FINDINGS.md`). Several land on the
interpreter optimisations this fork added, and four of those are compiled in on
this port, so they were live in the firmware.

`py/vm.c` — **the dict store fast path skipped the fixed-map check.** The general
path calls `mp_ensure_not_fixed()` and raises `TypeError`; the shortcut went
straight to `mp_map_lookup(..., ADD_IF_NOT_FOUND)`. A fixed dict is reachable from
Python as a builtin module's `__dict__`, and those tables can be in ROM, so this
turned a controlled exception into an assert or a write to read-only memory.

`py/objstrunicode.c`, `py/qstr.c` — **the single-character qstr cache indexed out of
bounds on malformed UTF-8.** The comment left in the code claimed `len == 1` proved
the byte was ASCII. It does not: a lone continuation byte, 0x80 to 0xBF, has the
high bit set but no further one bits, so the counting loop adds nothing and the
length stays 1. `qstr_from_char` then indexed its 128 entry table past the end, with
the assert compiled out. Both call sites test the byte now, and `qstr_from_char`
falls back rather than trusting the caller.

`py/map.c` — **an insertion that reused a tombstone did not bump the mutation
count.** Every other insertion does. The global-name cache keys its invalidation off
that counter, so a new global shadowing a builtin of the same name left the cache
returning the builtin.

`py/objstr.c` — `str(bytes, encoding)` searched the qstr pools even under
`MICROPY_OPT_STR_NO_INTERN`, which exists precisely to stop that.

`shared-bindings/custom/__init__.c` — `truncate_text()` counted accepted
**characters** and handed the result to a string constructor as a **byte** length, so
a multi-byte character was cut mid-sequence into malformed UTF-8. That was also the
producer that made the qstr cache overrun reachable from Python. The count was an
`int16_t`, and every result was permanently interned. It returns a byte offset past
the last whole character that fitted now, builds a plain string, and answers `""`
rather than the integer `0` for empty input.

`shared-bindings/displayio/TileGrid.c` — **this one caught a hole in a fix from the
previous batch.** The tile dimensions were validated after the zero-means-whole-bitmap
default had already been applied, so 65536 truncated to zero, became the bitmap's
size and passed. Validated as given now.

Then the memory-safety tier the audit recommends first:

`shared-module/displayio/OnDiskBitmap.c` — the bit depth came straight from the file
unchecked. Zero divides by zero computing `pixels_per_byte`; 9 to 15 make it zero and
the single-byte branch then takes `x % 0`; and from 40 up `bytes_per_pixel` is 5 or
more, so `f_read` reads that many bytes into a `uint32_t` local and walks the stack.
Only depths the file actually handles are accepted. Separately, `colors_used` is 32
bits in the file but the palette constructor, the size and the loop counter were all
16 bit, so a large declared count truncated differently in each place; it is clamped
to what the depth allows and the arithmetic is `size_t`. A palette declaring exactly
one colour allocated one entry and then wrote index 1 as well.

`shared-module/audiocore/WaveFile.c` — the final-block padding used the remainder
instead of the distance to the next word boundary, so a block of length 3 mod 4 grew
by three and wrote two bytes past the buffer, while 1 mod 4 grew by one and stayed
misaligned. A caller-supplied buffer half is also rounded down to a whole number of
words now, so the rounding always has room, and the 16-bit silent sample is written
bytewise because a sliced memoryview need not be halfword aligned.

`shared-module/audiocore/RawSample.h`, `WaveFile.h` and their bindings — the objects
kept only a raw pointer into the caller's buffer. A sliced memoryview hands over an
interior pointer that is neither block aligned nor at the head of a GC block, so once
the view and its source were dropped the collector could reclaim the samples during
playback. The owning object is stored and therefore traced. A deliberate resize of
the source can still move the storage; that is not closed.

`shared-bindings/custom/__init__.c` — eleven drawing entry points cast their argument
to a bitmap with no type check, so any other Python object was read and written as a
`displayio_bitmap_t`. And `draw_char()` checked only that the glyph list had six
items while deriving every later index from an unvalidated width and height, with a
width above 30 turning `0x7FFFFFFF >> (30 - width)` into a shift by a huge unsigned
value.

Verified across eight suites, 179 checks, none failing. Malformed BMPs at 0, 12, 40
and 64 bpp are refused with a canary intact beside them; a 70000-colour declaration
and a one-colour palette both load without writing past the allocation; five WAV
files chosen to land on every padding remainder play with their canaries intact; a
sliced memoryview handed to `RawSample` survives six collections under heap pressure;
twelve wrong-type arguments to the drawing functions raise instead of dereferencing;
and the seven earlier suites still pass.

### Sixth batch — divide by zero and non-terminating loops

The second tier of that audit's own fix order. All of these are values that reach a
divisor, a modulus or a loop bound without ever being range checked.

`shared-bindings/bitbangio/I2C.c`, `SPI.c` — `frequency=0` reached
`500000 / frequency`, and `configure(baudrate=0)` reached both a division and a
modulo by it. Both are validated to `1..500000` now, the upper bound being where the
bit-banged half-period would round to zero.

`shared-bindings/busdisplay/BusDisplay.c`,
`shared-bindings/framebufferio/FramebufferDisplay.c` — `refresh()` divided by
`target_frames_per_second` in the binding itself, so zero divided by zero. Anything
above 1000 produced a zero millisecond period and the common-hal refresh then took
`elapsed % target_ms_per_frame`, a modulo by zero. `minimum_frames_per_second` had no
range check either: negative silently meant "disabled" while a large value made a
zero maximum period, so nearly every elapsed millisecond raised `RuntimeError`.
Periods are whole milliseconds, so both are bounded at 1000.

The display constructor's `native_frames_per_second` is narrowed to `uint16_t` at the
common-hal boundary and then used as `1000 / native_frames_per_second`, so zero and
65536 both divided by zero, and 1001 upwards gave a zero period that makes
auto-refresh run on every background pass. Validated before the narrowing.

`shared-bindings/audiocore/RawSample.c` — `channel_count` was used unchecked: with
double buffering the length test divides by `bytes_per_sample * channel_count * 2`,
and with a single buffer the zero was stored and `get_buffer` later took
`channel % channel_count`. Negative and oversized values were narrowed into a
`uint8_t` before anything looked at them. Bounded to 1 or 2, and `sample_rate` to at
least 1.

`shared-module/audiofilters/Filter.c` — `filter_states_len` is a `size_t` but all
three loops over it counted with `uint8_t`, so 256 or more `Biquad` objects wrapped
the counter and the loop never terminated.

`shared-module/vectorio/Polygon.c` — the point count is a `size_t`, but `self->len`
is a `uint16_t` holding twice it and the validation loop counted with a `uint16_t`.
From 32768 points the stored length truncated; above 65535 the validation loop itself
never finished. Capped at what the stored length can represent.

Verified across nine suites, 204 checks, none failing.

**Two of these are not verified on hardware and should be treated as untested.** The
`vectorio` point cap needs a 32768-tuple list, which does not fit in this board's
heap — the test runs out of memory before reaching the check. And `audiofilters` is
not built into this firmware at all, so the `Filter` loop counters could not be
exercised. Both changes are small and were read carefully, but they have not been run.

### Seventh batch — sequence parsers and basic function

`shared-module/bitbangio/SPI.c` — **bit-banged SPI reported failure after every
successful write.** The pin protocol returns 0 on success, and three branches tested
`!digitalinout_protocol_set_value(...)`, so the binding raised `OSError` on every
transfer that used MOSI. Three other branches in the same file already tested
`!= 0`. This made `bitbangio.SPI` unusable, and it is now demonstrably fixed: 4, 64
and 3 byte writes all complete without an exception where they previously could not.

`shared-module/busdisplay/BusDisplay.c`,
`shared-module/epaperdisplay/EPaperDisplay.c` — neither init-sequence parser checked
what was left of the buffer before reading a command, a length, that many payload
bytes and an optional delay byte, so a short or malformed sequence read past the end
and sent whatever it found to the display bus. In the e-paper parser the extended
two-byte length was also assembled into a `uint8_t`, so the high byte vanished
immediately and the payload and every following command were then read at the wrong
offset. Both lengths are also validated against `UINT16_MAX` at the binding, where
they were being narrowed from `size_t` without a check.

`shared-bindings/busdisplay/BusDisplay.c` — `bytes_per_cell` is declared
`MP_ARG_INT` but was read as `.u_bool`. That is not a conversion to boolean; it
reads the inactive member of the argument union.

`shared-module/lvfontio/OnDiskFont.c` — the header field is the real bits per pixel,
1 to 4, which is what `read_bits()` is handed when a glyph is decoded. Using
`1 << bits_per_pixel` as the cache bitmap's storage depth therefore asked for 2, 4,
8 or 16 bits where 1, 2, 4 and 4 are needed, so **the glyph cache took two to four
times the RAM it should**. For a 1 bpp font it is now 1 bit per value instead of 2:
measured on the board, the 64-slot cache bitmap halves, and the supervisor's
240-slot terminal cache drops by about 4.3 kB on a 170 kB heap. The field is also
validated to 1..4, which it never was.

`shared-module/audiofilters/Distortion.c`,
`shared-module/audiodelays/PitchShift.c` — `memset(buf, 32768, ...)` writes only the
low byte of its value, and `32768 & 0xff` is zero, so the "silence" fill produced
zeros. For unsigned samples that is full negative deflection, not silence. And the
hard clip clamped to `+32768`, which becomes `-32768` on the cast to `int16_t`, so a
positive overdrive came out as a negative spike.

Verified across ten suites, 212 checks, none failing.

**The two audio effect files are not even compile-checked.** `audiofilters` and
`audiodelays` are not built into this firmware, so nothing in the normal build
touches them. Enabling them temporarily to get a compile pass broke qstr generation
and had to be reverted. The buffer types were confirmed by reading
(`word_buffer` is `int16_t *`, `length` is a sample count) and an explicit cast was
added so the fill cannot trip an overflow warning, but no compiler has seen either
edit. `BusDisplay`'s parser and `bytes_per_cell` are verified only indirectly, by the
board's own display still initialising — a second `BusDisplay` cannot be constructed
here. SM-18, the seven effects reading unsigned PCM without the offset-binary
conversion, was left alone: same unbuilt modules, and a larger change than these two.

### Ninth batch — found while testing the multi-tile fast path

Two in `shared-bindings/displayio/TileGrid.c`, both on the constructor, and the
second one is a board reboot.

`default_tile` went straight into a `uint16_t` field with no check at all, while
`tilegrid[i] = n` three hundred lines away validates its index against
`tiles_in_bitmap`. So the same illegal tile raised through one route and passed
silently through the other, where it renders as palette entry 0 rather than saying
anything. It truncated too, turning 65536 into 0 and -1 into 65535 — the same
narrowing class as `width`, `height`, `tile_width` and `tile_height`, all of which
this file already validates for exactly that reason. `default_tile` was simply
missed.

Choosing the upper bound for that check is what turned up the second one.
`displayio.Bitmap` accepts a zero width, and `bitmap_width % tile_width` is 0 for a
zero width, so the divisibility test passes and `bitmap_width_in_tiles` comes out
zero. The render loop then evaluates `tile % bitmap_width_in_tiles`. On this chip an
integer divide by zero is a panic, not an exception: **`displayio.Bitmap(0, 16, 2)`
in a TileGrid reboots the board on the next `refresh()`.** Confirmed by stepping it
in the REPL — `pred refresh` printed, `po refresh` did not, and the USB device
dropped. It is the same class the sixth batch was about; that batch fixed the grid's
own dimensions and the tile size, but not the case where the bitmap contributes the
zero. The constructor now rejects a bitmap holding no tiles with a message that says
so, rather than letting `default_tile` fail against an empty range and blame the
wrong argument.

Verified with nine checks: four out of range `default_tile` values rejected, legal
values and the default still accepted, both zero-sized bitmaps rejected, and
ordinary rendering unaffected. The full set is 258 checks across twelve suites, none
failing, and the TileGrid benchmark is unchanged.

### Eighth batch — compatibility and validation edges

The last tier of that audit's fix order.

`shared-module/displayio/OnDiskBitmap.c`, `.h` — **a BITMAPCOREHEADER was accepted
and then read entirely through Windows INFOHEADER offsets.** CORE stores 16-bit
width and height at bytes 18 and 20 and the bit count at byte 24; the parser read a
32-bit width from 18, a height from 22 and the depth from 28, and then read
compression from byte 30 — which for a 12-byte header is stack the file never
filled. Its palette is 3-byte RGBTRIPLEs, read as 4-byte RGBQUADs, which shifted
every colour and ran on into the pixel data. Every field of a CORE bitmap was wrong.
It is parsed where it actually lives now, and the header buffer is zeroed.

Width, height, the data offset and the stride all came out of 32-bit header fields
but were stored in 16 bits, so a valid top-down BMP — written with a negative
height — became a huge positive one, and a file whose pixel data starts past 65535
seeked elsewhere. And `read_word` promoted its high half to signed `int` before
shifting, so any value from 0x8000 up was undefined.

`shared-module/audiocore/WaveFile.c`, `.h` — the format chunk only had its *upper*
size bound checked, so a short or zero-length one was accepted and the unwritten
fields of an uninitialised struct were then validated and copied out; the short-read
test right below it had an empty body. Zero channels and zero bits per sample both
passed the "not too large" tests and divide by zero further along. A RIFF chunk of
odd length carries a pad byte that is not in its length, so skipping only the length
left the next tag read one byte off. `data_start` is a file offset that was stored
in 16 bits. And the parser demanded the literal bytes `WAVEfmt ` at offset 8, i.e.
`fmt ` had to be the very first subchunk — RIFF only requires it before `data`, so a
perfectly valid file with a `JUNK` or `LIST` chunk ahead of it was rejected. The
subchunks are walked now.

`shared-bindings/audiomixer/Mixer.c`, `shared-bindings/audiobusio/PDMIn.c`,
`shared-bindings/bitmaptools/__init__.c` — arguments narrowed before they were
checked. A mixer voice index of 256 or -256 became 0 and quietly drove voice 0;
`PDMIn`'s bit depth and oversample were narrowed to `uint8_t` before the
divisibility test, so zero passed and values 256 apart were indistinguishable; and
`boundary_fill`, `draw_line` and `draw_circle` narrowed their coordinates to
`int16_t` before any range check, so a value 65536 away from a legal one was
accepted as that one. All are validated as given now.

`py/vm.c` — the subscript fast paths this fork added tested bit 0 of the object to
decide "small int". That only holds for object representations A and C. In B and D a
qstr object has it too, so a run-time qstr could enter the integer path and have its
index read as a number. This port uses C, but `py/` is shared, so the test is a real
`mp_obj_is_small_int()` now.

Verified across eleven suites, 249 checks, none failing. CORE bitmaps at 4 and 8 bpp
load with the right dimensions and a correctly ordered palette, a top-down BMP reads
as 4x4 rather than 65532 rows, three WAVs with `JUNK`/`LIST` chunks ahead of `fmt `
(including one of odd length) open correctly, and the narrowing cases all raise
instead of silently acting somewhere else.

---

## 5. Measurements

240 MHz, best of several runs, loop overhead subtracted. "Before" is the stock
firmware where a stock number exists, otherwise the state right before the change
in question.

For what an operation costs *now*, rather than what a change was worth,
`MEASUREMENTS.md` has a per-operation sweep of the current firmware: about
seventy operations across calls, name access, arithmetic, subscripts,
containers, attributes, exceptions, allocation and collection, with the method
and its traps written down.

### Operations, cycles

| | before | after |
| --- | ---: | ---: |
| function call `f(1)` | 997 | **496** |
| method call `o.m(1)` | 1537 | **793** |
| builtin name lookup | 514 | **83** |
| builtin call `len(x)` | 422 | **325** |
| list index | 318 | **110** |
| list store | 317 | **110** |
| attribute store | 190 | **124** |
| `d["k"]` read | 422 | **143** |
| `d["k"]` store | 431 | **203** |
| bytearray read | 325 | **128** |
| bytearray store | 410 | **125** |
| `x & 3` | 174 | **109** |
| `x * 3` | 169 | **123** |
| `x // 3`, `x % 3` | 189 | **144 / 141** |
| `x << 3` | 193 | **127** |
| `if a_list:` | 235 | **167** |
| `if an_int:` | 102 | **80** |
| `str[3]` | 712 | **511** |
| `str[0]` | 1782 | **480** |
| `chr(100)` | 589 | **287** |
| `"ab" + "cd"` | 5605 | **1311** |
| `str(7)` | 5710 | **1502** |
| `"%d" % 7` | 5918 | **1646** |
| `",".join(...)` | 6217 | **1769** |
| iterating a str, per character | 1337 | **312** |
| iterating a list, per element | 246 | **206** |
| loop overhead per pass | 335 | **260** |

### Workloads

| | before | after |
| --- | ---: | ---: |
| bubble sort, 10 elements | 466.8 µs | **223.5 µs** |
| fib(12) recursive | 2629 µs | **1584 µs** |
| building a string 20× | 452 µs | **121 µs** |
| iterating UTF-8, per character | 125 µs | **3 µs** |
| full screen refresh | 89.8 ms | **8.8 ms** |
| small refresh, 60x38 dirty | — | **1.3 ms** |

### Later round — collector, strings, line I/O

A second pass, after the tables above were already taken. Nanoseconds per
operation for the first group, wall clock for the rest; board reset between the
before and after readings, best of several warm passes.

| | before | after |
| --- | ---: | ---: |
| `b[7]` on a `bytes` | 2492 ns | **1526 ns** |
| `"{}".format(str)` | 14709 ns | **9430 ns** |
| `f"{str}{int}"` | 25177 ns | **13255 ns** |
| `.x` read on a `Group` subclass | 5056 ns | **4415 ns** |
| `.x` write on a `Group` subclass | 4354 ns | **3916 ns** |
| `gc.collect()`, idle heap | 3.60 ms | **≤ 1.01 ms** |
| `gc.collect()`, 64 kB live `bytes` | 14.65 ms | **1.46 ms** |
| `for line in f`, text | 28961 µs | **11352 µs** |
| `for line in f`, binary | 26275 µs | **9307 µs** |
| `readline()` loop | 29663 µs | **12023 µs** |
| `readlines()` | 28655 µs | **10314 µs** |

The line I/O figures are over a 17892-byte file; `read()` of the same file is
about 9.5 ms, which binary iteration now matches.

Two numbers in these tables are worth distrusting. `read()` measured 8544 µs
before the `readline()` change and 9948–10589 µs after, on a path that change
does not touch — most likely variance in obtaining a 17.9 kB contiguous block,
since `read()+split` did not move, but a layout shift in `stream.o` was not ruled
out. Similarly `"{:>8}".format()` moved 14018 → 14364 ns across the f-string
change, which also does not touch it.

### HTTP response latency

Chasing
[Adafruit_CircuitPython_HTTPServer#46](https://github.com/adafruit/Adafruit_CircuitPython_HTTPServer/issues/46),
which reports request handling wandering between 40 and 250 ms at random and
blames `_receive_header_bytes()`. Reproduced here on `adafruit_httpserver` 4.8.2
with a `HttpClient` POST every 250 ms, 39 samples after warm-up:

| | min | p50 | p90 | max |
| --- | ---: | ---: | ---: | ---: |
| stock | 41.6 | **50.9** | 58.0 | 126.2 |
| `power_management = NONE` | 14.7 | **16.5** | 19.5 | 34.4 |

**The library is not the problem.** With `_receive_header_bytes()` instrumented to
time every individual `recv_into()`, a request from an ordinary HTTP client always
arrives as one `recv` of 0.4 to 4.4 ms. There is no waiting in that loop at all.

Two separate causes were found, and they are independent of each other.

**WiFi power save, which is most of it.** `wifi.radio.power_management` defaults to
`MIN`, so the radio sleeps between DTIM beacons and an arriving packet waits for
the next one. How long depends on where in the beacon cycle it lands, which is the
randomness the issue describes. Latency against the spacing of the requests, power
save on throughout:

| request spacing | awake window 50 ms | 60 ms |
| --- | ---: | ---: |
| 30-45 ms | 15-17 ms | 15-17 ms |
| 50 ms | 35.0 ms | 38.6 ms |
| 65 ms | **122.6 ms** | **41.2 ms** |
| 75 ms | **121.8 ms** | **25.7 ms** |

The cliff sits exactly at `ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME` and moves when
that is changed. Below it the radio never sleeps and latency equals the
power-save-off figure. See `espidf.wifi_sleep_min_active_time()` in section 3 for
the dial this exposes.

**A request split across two TCP segments, which is the other half and is what the
issue's own evidence shows.** The reported `157 ms / 1 ms / 152 ms` really is
inside the receive loop. It happens when the client sends headers and body as
separate segments with Nagle enabled: the body is held until the headers are
acknowledged, and lwIP defers that acknowledgement to its fast timer,
`TCP_TMR_INTERVAL`, which is 250 ms.

```
H 128B/579us        headers, half a millisecond
B 7B/243988us       body, 244 ms
```

| raw TCP client | ms |
| --- | ---: |
| headers and body in one write | 11 |
| body written separately, Nagle | **256, 256, 257, 257** |
| body written separately, `TCP_NODELAY` | 12-16 |

`HttpClient` and `curl` never trigger this because they write the request in one
piece; it had to be produced deliberately with a raw socket. The fix belongs on
the client.

**Why the firmware cannot fix that second half by configuration.** Lowering
`TCP_WND_UPDATE_THRESHOLD` so that the application's read carries the
acknowledgement out looks like the obvious answer and does nothing, because the
value it is compared against comes from `tcp_update_rcv_ann_wnd()`, which has its
own hardcoded gate of `LWIP_MIN(TCP_WND / 2, pcb->mss)` -- 1440 bytes here. A 128
byte read does not move the right edge of the window that far, so the function
returns zero and the threshold is never reached. The gate is not a configuration
macro, so there is no route to it short of patching lwIP inside the IDF submodule.

Two things this does not establish. Current draw was not measured, only duty
cycle, so the power side of the trade is reasoned rather than shown. And the
issue was filed against a Pico W while all of this was measured on an ESP32-S3;
the CYW43 has its own power save and the mechanism is the same in kind, but none
of these numbers carry over to it.

---

## 6. Not done

### `usb_video` — tried, bricks USB, left off

Enabling `CIRCUITPY_USB_VIDEO` on this board takes the whole USB device down.
Worth writing up because the failure mode is nasty and the diagnosis is not
obvious.

What it is, first, because the name misleads: it does **not** capture from a
camera. It makes the board *appear to the host as a webcam* and streams a
framebuffer out, so the host can see what the board is drawing.

Two separate walls, in order:

**Endpoints.** The S3 has 5 IN endpoints and they are all spoken for: 1 control,
2 CDC console (notification + data), 1 MSC, 1 HID. UVC needs a sixth and the
build drops to safe mode with "USB devices need more endpoints than are
available". Compiling MIDI out does **not** help — MIDI is already disabled at
run time by `CIRCUITPY_USB_MIDI_ENABLED_DEFAULT=0`, so it was never holding an
endpoint. One has to be freed at run time instead, from `boot.py`:
`usb_hid.disable()` or `storage.disable_usb_drive()`.

**The descriptor.** With an endpoint free, Windows then rejects the composite
device outright: *"This device cannot start"*, problem code 10. That takes the
serial port and the CIRCUITPY drive with it, so the board can only be recovered
through ROM download mode — holding G0 while tapping reset. There is no software
way back.

Likely cause, untested: `supervisor.mk` asks for **bulk** UVC streaming
(`CFG_TUD_VIDEO_STREAMING_BULK=1`) and Windows only really supports
**isochronous** UVC. That flag is now a board choice, `CIRCUITPY_USB_VIDEO_BULK`,
defaulting to 1 so upstream behaviour is unchanged; this board sets it to 0 ready
for another attempt. Whether isochronous fixes it was never verified — the board
was in the failed state by then.

Supporting evidence for it being a port problem rather than a mistake here: the
CircuitPython docs list compatibility as "predominantly RP2040 and RP2350-based
devices", and the module documents itself as experimental.

Memory, for whenever it does work: `4 × width × height` bytes, as two buffers —
RGB565 that `displayio` draws into, and YUYV that actually goes out over USB,
converted before each frame. 128×96 is 48 kB; the display's own 240×135 would be
130 kB, which does not fit. The 2 bytes per pixel of YUYV are fixed by the UVC
uncompressed format, so a 4-colour indexed bitmap cannot be sent as such — but
the RGB565 buffer could be dropped by converting straight from an indexed
`displayio.Bitmap`, which would take 128×96 down to about 27 kB.

### Native code emitter — attempted, removed entirely

`@micropython.native` and `@micropython.viper` do not work on this board, and the
scaffolding that was built to make them work has been **taken back out**. Nothing
of it remains in the tree: no `MICROPY_EMIT_XTENSAWIN`, no `MP_PLAT_ALLOC_EXEC`, no
executable pool in `mphalport.c`, no `CIRCUITPY_NATIVE_CODE_POOL_SIZE`, no
`espidf.exec_pool_info()`. `MICROPY_NLR_SETJMP_BUILTIN` is now unconditionally `1`
rather than being conditional on the emitter being off.

Worth recording why, because the reasoning is easy to walk into again.

Generated code has nowhere to live. The internal SRAM is mapped twice, once for
data and once for instructions; a pointer from the garbage collected heap is the
data mapping, and executing it faults — verified, the board went to safe mode with
"Hard fault: memory access or instruction error". `heap_caps_malloc()` with
`MALLOC_CAP_EXEC` is the usual answer, and it reported **0 bytes** of executable
memory free on the cardputer even before the interpreter heap was created.

Reaching the code through the instruction alias of the same SRAM would be the way
through, but the collector traces the data address, so the alias cannot simply be
stored in the function object — it would have to be converted at the call sites in
`objfun.h`.

And it would cost something even if it worked. Emitted code reaches `setjmp`
through `mp_fun_table`, and `__builtin_setjmp` cannot be called through a pointer
because the buffer it fills is only valid in the frame that called it. So the
emitter rules out `MICROPY_NLR_SETJMP_BUILTIN`, which is worth about **300 cycles
on every Python call** — paid by all code, to benefit the decorated functions that
do not run.

Two claims made about this earlier in the fork's history were wrong and are
corrected here: the scaffolding never enabled `@micropython.native` (the emitter
was off the whole time), and removing it was not required for anything else to
compile.

### Considered and rejected

`MICROPY_STACKLESS` was measured as a regression: calls +26 %, fib +20 %.

Integer division on this core costs 6 cycles for a variable divisor and 0 for a
constant, which is why the division in `mp_map_lookup()` was left alone.

The garbage collector used to be the largest remaining cost, at 3 ms with 38 kB
live and 6.5 ms with 76 kB. Those figures are superseded: two bugs in `gc.c` and
the string payload change, all described in section 1, took an idle collection
from 3.60 ms to under 1.01 ms and one with 64 kB of live `bytes` from 14.65 ms to
1.46 ms. What is left is structural — the cost of a single allocation still
varies about eightfold with fragmentation, and improving *that* means
generational or incremental collection, which is a project, not a tweak.

**A 32 kB instruction cache was measured and not kept.** The ESP32-S3 supports
one and nothing in the tree ever set it for this chip — the S2 defaults file sets
its own, the S3 one does not, so the IDF default of 16 kB applied. Built and
measured both ways, each after a hard reset:

| | tight loop | multi-module loop | `compile()` 22 kB | cold import | free heap at boot |
| --- | ---: | ---: | ---: | ---: | ---: |
| 16 kB | 1017 ns | 15238 ns | 87.5 ms | 670.9 ms | 144416 B |
| 32 kB | 997 ns | 14425 ns | 78.6 ms | 595.3 ms | 128032 B |

So 2 % on a tight loop and 5–11 % on anything spanning real code, for exactly
16384 B of the D/IRAM pool the Python heap is carved from. The largest obtainable
block is 30720 B either way — that limit is set elsewhere — but total free heap
is 11 % lower, and a sequence of three `compile()` calls followed by an import
fits at 16 kB and raises `MemoryError` at 32 kB. Kept at 16 kB for the headroom;
the speed is the better buy only on a board that is not close to its memory
limit. The value is now set explicitly so the decision is visible.

**A larger GC mark stack moves the cliff instead of removing it.**
`gc_mark_subtree` pushes one entry per unmarked child while scanning a parent, and
`MICROPY_ALLOC_GC_STACK_SIZE` is 128, so a container of more than that many
objects overflows on the first parent it touches and `gc_deal_with_stack_overflow`
then rescans the used region. Timing `gc.collect()` from Python against a flat
list of N small objects locates it exactly: 1.53 µs per object at N=120, 4.29 µs
at N=128, flat at ~3.5 µs above — a 2.4× step at precisely the configured size.
That is the evidence an earlier review asked for and could not get from the
source.

Raising it to 512 was built and measured. Collections with 128-384 live children
got 1.6-1.9× faster (128 children 732 → 397 µs, 256 children 1068 → 580 µs), but
nothing above 512 improved and those were a consistent 4-8% *slower* across two
runs, for 3072 B of RAM. Left at 128: the win is a narrow band and the change
does not address the mechanism. What would is a cheaper overflow path — the
rescan is O(used heap) per round — and that is a project, not a tweak.

One measurement artefact worth recording, because it will mislead someone. With
the larger stack the profile showed a 60 kB bytearray surviving two collections
after its only Python reference was dropped, then behaving normally in a later
row. That is conservative collection: 3 kB more BSS shifted the C stack, and a
stale copy of the pointer happened to land in a slot that is scanned. It is not a
leak and not caused by the stack size as such — but it means any before/after
comparison of `mem_free` across a change in BSS size is unreliable.

**Moving the computed-goto table into internal RAM did nothing.** `entry_table`
in `py/vmentrytable.h` is tagged `PLACE_IN_DTCM_DATA`, which on espressif expands
to nothing, so the 1 kB table lands in DROM at `0x3c18ec1c` — external flash
mapped as data — and every dispatch indexes it through the data cache. Giving the
macro a real definition for `ESP_PLATFORM` moved it to `0x3fc9e300` in internal
DRAM at a cost of exactly 1024 B of SRAM1, confirmed by `nm` and the size report.

Measured on six deliberately dispatch-bound loops, three runs after a reset
against two before: nothing moved. `noop` 1068/1053 → 1038/1053/1053, `arith`
2960/2930 → 2975/2960/2930, and a bitwise loop returned 1953 ns in all five runs.
Resolution here is about 15 ns and every difference was one or two steps, in both
directions.

The reasoning that suggested it was wrong in the same way as the instruction
cache argument: the table is small and hot, so it stays resident in the 32 kB
data cache and a tight loop touches one or two of its lines. There was never a
steady-state miss to remove, only a compulsory one, and moving to RAM is as
powerless against that as a bigger cache. Reverted; the 1 kB is worth more than
the nothing it bought.

`SELECTIVE_EXC_IP` (`py/vm.c:334`) was costed and left alone. With it at 0,
`code_state->ip = ip` runs on every dispatch, and since
`MICROPY_OPT_COMPUTED_GOTO_SAVE_SPACE` is 0 that store is inlined at every
opcode site. It is one cycle per bytecode: against a measured 244 cycles for an
iteration of an empty loop executing three bytecodes, about 1.2 %, and the share
falls as the work per bytecode rises. Upstream's own note claims 1–3 % for
~360 bytes. Turning it on requires auditing 29 CircuitPython divergences in that
file for raise sites without `MARK_EXC_IP_SELECTIVE()`, each of which would leave
a stale `ip` and a traceback pointing at the wrong line — a silent diagnostic
regression for a gain at the edge of measurable.

Below about 25–30 cycles per bytecode there is nothing left without combining
common opcode pairs into single instructions, which is a change to the compiler
and the VM both.

---

## 7. Tests

Python test suites, run on the board. 1000+ cases in total, all passing.

Run them one at a time with the board reset in between. Several suites allocate
several kB and will raise `MemoryError` at the end of a long chain purely from
accumulated session state, then pass on their own — see section 0.

| File | Cases | Covers |
| --- | ---: | --- |
| `str_test.py` | 141 | Non-interned strings as dict keys, set members, attribute names, `__import__`; all sign combinations of `//` and `%`; overflow into big integers; shifts |
| `readline_test.py` | 48 | `readline()`, `readlines()` and iteration against `read()`; the file position after a line, so that a following `read()` or `tell()` is right; `readline(n)` cutting mid-line; a line longer than the block; empty file, empty lines, no newline at EOF; text and binary |
| `subprop_test.py` | 20 | Native properties reached through a Python subclass: read, write, setter type checking, a Python `@property` on the subclass, one shadowing a native property, ordinary instance attributes, missing attribute, absent deleter |
| `negidx_test.py` | 53 | Negative and out-of-range subscripts on list, tuple, bytearray and bytes: last valid index, one past each end, indices below `-len`, empty containers, writes, byte overflow, `del`, slices, non-integer keys, a subclass override |
| `digitalio_test.py` | 25 | `get_pull()` reporting all three states and round-tripping them, and a Python subclass of `DigitalInOut`: reading value, direction and pull, setting them, `drive_mode` raising on an input as documented rather than rebooting, a subclass with its own state and method, `deinit`, and the direct type as a control |
| `delattr_test.py` | 24 | `del obj.attr` against the VM store fast path: read and delete twice, slot reuse afterwards, several names deleted out of order, an instance attribute shadowing a class one, names never set, `setattr`/`getattr` round trip, and set-delete-read in a loop |
| `gcstress_test.py` | 50 | Collector integrity across heap growth and area removal: witness structures of lists, tuples, strings, bytes, dicts and a back reference, verified after six grow/shrink cycles, a 600-deep chain, and interleaved allocate-and-collect. Guards the area span that `gc_get_ptr_area()` rejects against — too narrow a span would sweep reachable objects silently |
| `dict_test.py` | 107 | Keys of every type, `KeyError` carrying the key, rehashing past 200 entries, `OrderedDict`, subclasses, deletion from list, dict and bytearray, truth of every container |
| `builtin_call_test.py` | 98 | Wrong argument counts for fixed and variable signatures, keywords where unsupported, custom `__next__`, iteration through `__getitem__`, `break`/`else` |
| `ba_test.py` | 82 | bytearray read and write, out of range, overflow, slices, memoryview, `array.array` staying on the general path |
| `fastpath_test.py` | 53 | Indexing, method calls, attribute stores, subclasses with overrides |
| `char_test.py` | 50 | The whole ASCII table by index and by iteration, UTF-8 characters, memory returning after 500 different ones |
| `arith_test.py` | 52 | Small integer arithmetic |
| `call_test.py` | 48 | Calling conventions |
| `shadow_test.py` | 47 | Invalidation of the global name cache |
| `timer_multi_test.py` | 34 | Several timers at once, restarting, the 8 limit, slot reuse |
| `timer_test.py` | 19 | Timer intervals, exceptions in callbacks, surviving collection, `wifi.Monitor` |
| `esp_test.py` | 70 | Partition read/write/erase/mmap, refusing the running partition, heap untouched by mmap, power management, and CSI: every metadata key, `readinto` allocating nothing against `packet()`, amplitudes in C versus a Python reference, the source filter passing its own AP and blocking a fabricated address, the LTF and shift settings, argument checking on an empty queue, and `wifi.Monitor` silencing CSI |
| `esp2_test.py` | 40 | NVS int/str/bytes, type changes, persistence across reopen, namespace isolation, check_heap, BLE PHY, raw 802.11 tx |
| `pin_test.py` | 21 | pin_status direction/pull/level/free through a real allocate-and-release, the board's claimed pins, vendor IE set and clear |
| `reg_test.py` | 50 | registers.extract/insert: fields, signed, byte order, multi-byte, round-trip, out-of-range, speed vs Python |
| `map_test.py` | 25 | RegisterMap cache none/full, batch transaction counts, sync/sync_all staleness, read-modify-write, 16-bit signed |
| `map2_test.py` | 28 | Attribute access, named values, scale/offset to physical units, read-only, a worked MPU6050-style driver |
| `exc_test.py` | 17 | Exceptions from depth, generators, `with`, under GC pressure |

Benchmarks: `bench_interp.py` for the main table, `bench5.py` to `bench10.py` for
the decompositions quoted above, `viper_test.py` for when the emitter works.

---

## 8. Already in CircuitPython

Found while looking for things to add, listed because they are easy to miss.

`wifi.Monitor` and `wifi.Packet` already implement promiscuous mode — verified on
the board, 31 frames in 1.5 s with none lost. `bitmaptools` already has
`draw_line`, `draw_circle`, `draw_polygon`, `fill_region`, `boundary_fill`,
`blit`, `rotozoom`, `arrayblit`, `alphablend`, `replace_color` and `dither`, which
overlaps much of `shared-bindings/custom/`. Also enabled in this build and easy to
overlook: `memorymap.AddressRange` for reading and writing arbitrary addresses
from Python, `espulp` for the RISC-V coprocessor, `ulab` for array maths,
`aesio` and `hashlib` on the hardware accelerators, and a settable
`microcontroller.cpu.frequency`.

`MICROPY_OPT_MAP_LOOKUP_CACHE`, `MICROPY_OPT_LOAD_ATTR_FAST_PATH` and
`MICROPY_OPT_COMPUTED_GOTO` are already on through `CIRCUITPY_FULL_BUILD`, so
there is nothing free to gain by flipping them.

Not part of this work: `shared-bindings/custom/` is a separate module of your own.

---

## 9. Upstream merge, 2026-08-29

`adafruit/main` merged as `31a146df91`, moving the base from `10.3.0-alpha.4` to
`10.3.0-rc.0`. 127 commits, 86 of them not merges.

The merge was clean. Exactly one file had been changed on both sides —
`ports/espressif/common-hal/_bleio/Characteristic.c` — and the two changes are in
different functions with no interaction: ours accepts indications in
`characteristic_on_ble_gap_evt()`, which is the client receiving data, while
theirs sets `BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC` in
`common_hal_bleio_characteristic_construct()`, which is the server deciding who
may write the CCCD. Git produced no conflict markers anywhere in the tree.

The two `.mk` files carrying the local `custom` module hunks have to be stashed
first, because upstream also changes both of them to register `picogame`. They
reapply without conflict — `CIRCUITPY_CUSTOM` sits between `COUNTIO` and
`DISPLAYIO`, `picogame` lands under `P`.

### What arrived that matters on this board

**Dirty-rectangle filtering in `shared-module/displayio/Group.c`.** The public
`displayio_group_get_refresh_areas()` became a wrapper around the old body, now
`group_get_refresh_areas_impl()`, and passes the collected list through
`filter_out_redundant_areas()`, which unlinks any area wholly contained in
another. The case it is aimed at is a text label being rewritten: the group
dirties the whole label region through `item_removed` and then adds one area per
glyph, every one of them inside that region. Dropping the covered ones is
lossless, since the survivor refreshes the same pixels in fewer transfers.

This is a different axis from section 2, which makes a refresh of a given area
cheaper to compute and to send. This one reduces how many areas are sent at all,
so the two compose rather than overlap. Note that the nodes are owned by the items
— `TileGrid` reuses `dirty_area` as its tile-space accumulator between frames —
so the filter relinks `.next` and never touches coordinates.

Upstream also moved this logic out of `busdisplay` and into `Group` so that it
exists once instead of once per display type, which is why the firmware came out
**80 bytes smaller** (2047664 → 2047584) despite gaining a seventy-line function.

**`shared-module/sdcardio/SDCard.c`** — correct capacity for SDXC cards above
32 GB. This board has a slot and `CIRCUITPY_SDCARDIO = 1`.

**`supervisor/shared/web_workflow/web_workflow.c`** — the listener socket calls
are checked for failure instead of being assumed to succeed.

**`supervisor/shared/safe_mode.c`** — boot button gets LED feedback, and a press
no longer leaks from one polling interval into the next.

**BLE, several genuine defects.** Heap handler removal stopped after the first
removal; the disconnect wait in `bleio_adapter_reset()` ran zero iterations;
`common_hal_bleio_packet_buffer_deinited()` never returned true; the serial RX
buffer size was overstated fourfold. Alongside those, writing the CCCD of an
encrypted characteristic now requires encryption, which previously let an unpaired
central subscribe without ever being made to pair.

### Not taken

`picogame`, a new game engine module of about 7400 lines across 35 files including
an ESP32 backend, arrives with the merge but stays off: `CIRCUITPY_PICOGAME = 0`
for this board. Nothing was built for it, so neither the flash use nor the qstr
pool changed. Turning it on is a separate decision.

Everything under `ports/zephyr-cp` (101 files), `ports/nordic`, `ports/raspberrypi`
and `ports/silabs` came along and costs nothing, because none of it is compiled
here.

### Verification

| | |
| --- | --- |
| MicroPython's own suite against `ports/unix` | 934 tests, 27609 cases, no failures — the same counts as before the merge |
| Regression tests on the board | 220 cases across six files, no failures: `delattr_test` 24, `negidx_test` 53, `subprop_test` 20, `digitalio_test` 25, `readline_test` 48, `gcstress_test` 50 |
| Display, on the board | `tg_test` all eleven sections including overlapping layers and transparency under overlap, `zerotile_test` 9 of 9 |

Spot-checked in the merged tree: the negative-subscript helper, the GC pool
bounds, the digitalio subclass accessor, the chunked readline, the string payload
allocation and the `bytes` subscript option are all still in place, and `Group.c`
holds upstream's `filter_out_redundant_areas()` alongside this fork's
`draw_get_dirty_area()` fix from section 4.

---

## 10. Audit of shared-bindings and shared-module, 2026-08-30

45 commits. Six reviewing agents were pointed at the 349 files this board
actually compiles, one subsystem each, and asked for correctness defects *and*
speed-ups with the mechanism stated rather than the impression. They returned
about 120 findings; 65 were checked before any code was touched and every fix
below was built, flashed and tested against both the case that provoked it and
the ordinary use of the same call. The full record, including what was refuted
and what was measured and then left alone, is `AUDIT-FINDINGS.md` section 14.

### Why the agents and not the analyser

Section 13 of `AUDIT-FINDINGS.md` reported that static analysis found nothing.
That was wrong: it found nothing because it never ran. `-fanalyzer` under
`-fsyntax-only` reports nothing at all, and under **any** optimisation level it
also reports nothing on this compiler — measured by planting a leak and a double
free and bisecting all 596 build flags. The earlier artefacts are 544 lines of
`error: unknown type name`, not findings. With a harness that plants a bug and
refuses to proceed unless the analyser reports it, 349 files at `-O0` produced six
warnings, all six verified false positives.

More to the point, an analyser cannot see that a loop recomputes an invariant or
that a data structure is quadratic. Every performance result below came from
reading the code and then measuring it.

### Four of them were this fork's own

The part worth the exercise. None was found by the earlier static pass.

| | |
| --- | --- |
| `digitalio` | The native-subclass conversion reached eighteen of twenty entry points. `switch_to_output` and `switch_to_input` still cast `pos_args[0]` directly, so on a subclass `self->pin` read as `0x1` and the board rebooted. Section 4 of this document had claimed all twenty were converted |
| `bus_core` | The window cache was invalidated from exactly one place. A raw `bus.send()` carrying CASET, or a panel reset, left it claiming a window the controller had forgotten |
| `bitbangio` | The divide-by-zero fix also added a ceiling of 500000, on reasoning that does not hold — the half-period is already rounded up to 1 — so it rejected 1 MHz and above, which is what `adafruit_bus_device` asks for |
| `busdisplay` | `buffer_size` was recomputed only when the dirty area did *not* fit, so an ordinary partial redraw still put 8192 B of buffers on a 24576 B stack |

### Crashes reachable from ordinary Python

Nine, all reproduced on the board and all now raising instead: `IPv4Address(None)`
followed by a print; a `getpass` prompt containing a per-cent sign; `randint` over
the full integer range, which spun with no interrupt check; a Python subclass of
`Warning` under `simplefilter("error")`; seven bytes of msgpack; a
self-referential list packed with msgpack; a HID OUT report whose first byte is
zero; and two in the board owner's own `custom` module — `rounded_rect` with a
negative x, and `draw_ellipse` with a large centre, which needed a hard reset.

### Reads and writes outside the allocation

Six: a negative `start` below `-length` in `buffer_helper`, which reaches `busio`
as well as `bitbangio`; `width * height` overflowing in `TileGrid`; an
advertising-data length byte driving an unbounded `memcmp`; GET_REPORT copying a
host-chosen length out of a report-sized block, sending heap contents to the host;
a `struct` repeat count read as a small int when it is a big-int object; and a
MIDI parser that recorded an error and then read past the buffer anyway.

### Silently wrong results

Twenty-six. Among them: `ip_address("300.1.1.1")` returned `44.1.1.1`; `aesio` in
CTR mode did not round-trip when the data was split into chunks; the mask in
`bitmapfilter` did the opposite of what four of its five docstrings say;
`colorwheel(-1)` returned −768; `registers.extract` never sign-extended a
full-width field; `getaddrinfo`'s `proto` argument could not be passed at all
because its slot was named `port`; a transparent vectorio shape drew black and hid
the layers beneath it; a tiled background redrew only its first cell; and a failed
`chdir` left `getcwd()` reporting a directory that does not exist.

### What the measurements said

Six changes, each measured on the board before and after:

| | before | after | |
| --- | ---: | ---: | ---: |
| `bitmaptools.boundary_fill`, 160x120 | 12065.78 ms | 25.88 ms | **466x** |
| `jpegio` decode into a 640x8 strip | 224.75 ms | 11.48 ms | **19.6x** |
| `jpegio` decode into 96x72 | 225.89 ms | 40.76 ms | 5.5x |
| `time.localtime` | 108.1 µs | 36.7 µs | 2.95x |
| `audiomixer` share of the core | 4.07% | 1.81% | 2.25x |
| `OnDiskBitmap` redraw, 48x64 | 12.94 ms | 7.51 ms | 1.72x |
| `registers.extract` heap per call | 32.0 B | 0 B | — |

`boundary_fill` was quadratic: it kept its frontier in a Python list and popped
from the front, so the cost per pixel rose from 76500 to 150822 cycles as the area
grew. Afterwards it is flat at about 320, which is what says the quadratic term is
gone rather than merely smaller. `jpegio` decoded every image in full because both
of its early exits reduced to a comparison the area validator already excludes —
that a 96x72 output and a 640x8 output took the same time is the finding on its
own. The mixer was calling the ROM soft-float divider twice per output word inside
an audio callback; the same function is now 23% smaller, which matters against a
16 kB instruction cache.

### Measured and deliberately not done

`struct` walks its format three times, worth about 10% — against a measurement
spread of 48%. `fill_region` and `rotozoom` do per-pixel work worth perhaps 1.5x
to 2x, against spreads of 258% to 516%. Those are harness problems rather than
code problems and the findings stand, but a change made on a number that cannot be
demonstrated is a change made on faith. See section 0 on why benchmarks in one
REPL session drift.

### Three lessons that cost time

**Only matched pairs mean anything.** Three separate measurements were inflated by
comparing a "before" from one script against an "after" from another. The mixer
first appeared to get *worse*; `localtime` was written up at 4.6x and is 2.95x. The
firmware layout shifts when its size changes, and the reference workload moves with
it.

**A fix can be broken in exactly the way the bug was.** The first attempt at the
`warnings` fix used `mp_raise_type_arg`, which carries the same assert and calls
`mp_obj_exception_make_new` directly — a native category worked and a Python
subclass still killed the board.

**Read the convention, then measure it.** `timeutils_calc_weekday` numbers the week
from Sunday and the decomposition it replaced numbers it from Monday. The formula
looked right; every weekday came out one too high until five anchor dates caught
it.

### Still open

`aesio` CTR keeps no keystream offset between calls; `ssl.check_hostname` is stored
and never consulted; `Group.sort()` marks nothing dirty and bypasses the readonly
flag. The first two want a decision about changing behaviour, and the third wants a
new shared-module entry point. The `bitmapfilter` mask polarity is a fourth: the
code and four of its five docstrings disagree, and either can be made to match the
other.

## 11. ESP32-C5 port and dual-band Wi-Fi, 2026-09-03

A new build target, `seeed_xiao_esp32c5`, for the Seeed Studio XIAO ESP32-C5:
single-core RISC-V, Wi-Fi 6, and the first part in this fork with a 5 GHz radio.
That last point is the reason it exists — 802.11ax channel estimates are what the
CSI work was heading towards, and they only appear on a Wi-Fi 6 part.

### What the port needed

`peripherals/esp32c5/pins.{c,h}` for 29 GPIOs with ADC1 on GPIO1-6, an
`esp-idf-config/sdkconfig-esp32c5.defaults`, the board directory itself, and a C5
block in `ports/espressif/Makefile` for the 0x2000 bootloader offset, the ROM
linker scripts and the chip component list. `CFG_TUSB_MCU=OPT_MCU_ESP32C5` is set
even though this part has no USB OTG: only the USB Serial/JTAG controller, which
is also the only console.

### The board stopped answering, and it was not our code

Opening and closing the serial port from Windows left the board enumerated on USB
but answering nothing — no panic, no reboot, no output — until power was pulled.
It took a JTAG halt to see what was really happening, and the answer was that
**CircuitPython was not running at all**: `mtvec` pointed into ROM, not our IRAM,
and the PC sat unchanged across halts a second apart at `0x4003b10e`, inside three
instructions that poll a register and branch back.

The register was `PCR_UART0_CONF_REG`, the bit was `PCR_UART0_READY`, and the
caller was the ROM's `Uart_Init`. `PCR_UART0_SCLK_EN` resets to 1 and the ROM
bootloader relies on that, but `esp_perip_clk_init` clears it at startup on any
build whose console is not UART0 — which is every CircuitPython build. Peripherals
survive a reset that only restarts the CPU, so the ROM then boots into a wait that
can never end.

Two things raise exactly that kind of reset: a JTAG CPU reset, and the USB
Serial/JTAG controller when a host asserts DTR/RTS on opening the port. Windows'
`usbser.sys` does; the Linux `cdc_acm` driver does not, which is why the same
50-cycle test failed on the third try from one host and passed 50 of 50 from the
other. The fix is two register writes at the top of `port_init`, C5 only.

Verified by putting the mechanism itself back: a JTAG CPU reset used to hang the
board every single time and now boots straight into the firmware.

Two measurement traps cost most of the time here, and both produced findings that
looked real. OpenOCD leaves the core halted unless the session ends with an
explicit `resume`, so every probe afterwards times out exactly like the fault
being chased. And with memory protection enabled it resets the chip on attach, so
readings describe a freshly booted board rather than the one that failed.

### Scanning above channel 14

`start_scanning_networks()` validated its channel arguments against 1-14 and the
espressif backend walked a hardcoded 2.4 GHz pattern, so a dual-band part could
never report a 5 GHz network. The range is now 1-165 and the pattern carries the
5 GHz channels behind `CONFIG_SOC_WIFI_SUPPORT_5G`, non-DFS first so a scan cut
short still covers where most access points sit. The defaults are untouched, so
nothing changes for a caller that does not ask.

One behavioural change came with it: a channel the current country setting
disallows makes `esp_wifi_scan_start` fail, and the old code ended the whole scan
there. It now moves to the next channel, or a single blocked 5 GHz channel would
hide every network above it.

### Measured on the board

A scan of 1-165 takes 9.4 s and finds both bands; the default 1-11 scan returns
the same five networks it did before. Associated to a 5 GHz AP on channel 36 by
BSSID, `espidf.CSI` delivered **108 records/s with none lost**, 404 of 432 in
HE-SU format at **490 bytes — 245 subcarriers**, against 106 bytes for the 11g
frames mixed in. That is the HE branch working on real hardware, and it confirms
raising `ESPIDF_CSI_MAX_BYTES` to 512 on HE parts: the 128 used elsewhere would
have truncated every record.

Generating that traffic needs care. UDP to a *closed* port on the gateway makes it
answer ICMP port-unreachable, which lwIP reports as `EPIPE` on the next send and
leaves the socket dead; an open port avoids it entirely.

### Still open on this board

`canio` wants a different `twai_ll` API: the C5 has TWAI-FD, whose acceptance
filter is a different model rather than a renamed one, and testing it needs a
transceiver. Everything else listed here when the port was new — BLE, PSRAM,
`_reset_pin()` — is covered in the sections below, together with the diagnoses
that turned out to be wrong.

### Pin reset, and a comment that was wrong

`_reset_pin()` carried three ESP32-C5 crutches from bring-up, the outermost of
which made the whole function a bare `return`. All three were written around a
guess -- that the pull-up and pull-down calls dispatch through
`rtc_gpio_is_valid_gpio()` into an `abort()` on this part. That guess does not
survive reading the code: `GPIO_RTCIO_ARE_INDEPENDENT` is 1 on everything except
the original ESP32, so the RTC branch is unreachable here, and `rtc_io_num_map`
for the C5 is correct.

Removing the crutches locked the chip up on every boot, with no output at all.
Markers through `esp_rom_printf` showed every one of the 29 pins completing every
stage of the reset, and the lockup arriving only afterwards -- so the fault was
not in `_reset_pin` at all. Bisecting the pin range narrowed it to a single pin:
**GPIO16**, which `IO_MUX_GPIO16_REG` shows is `PERIPHS_IO_MUX_U_PAD_SPICS0`, the
SPI flash chip select. GPIO15-22 carry the whole flash and PSRAM bus on this
part, and none of them were in `pin_mask_reset_forbidden`; the comment there said
the flash was on dedicated pads outside the GPIO range, which is simply not true
of the C5.

The failure is invisible by construction, which is why it took so long. Dropping
the flash chip select does not stop execution: the core keeps running out of the
instruction cache, so the pin loop finishes and hundreds of instructions retire
before the first cache miss faults. The panic handler then cannot run either --
it lives in flash -- so the second fault turns into `CPU_LOCKUP` and the chip
reboots reporting only a saved PC inside `esp_panic_handler_feed_wdts`. Nothing
is ever printed, on any console setting.

Worth keeping in mind for the next port: on this part a panic handler that never
speaks means flash, not a broken console.

### PSRAM

8 MB of quad PSRAM at 80 MHz, which needed one portability fix:
`common-hal/espidf/__init__.c` used `SOC_EXTRAM_DATA_SIZE`, and the C5 defines
only `SOC_EXTRAM_DATA_LOW`/`HIGH` without the convenience macro the S3 has. It is
now derived from the bounds when the target does not provide it.

The heap goes from roughly 153 kB to **8.26 MB**, and a 2 MB buffer written and
read back verifies the bus rather than just the reported size. Enabling it also
depends on the pin fix above: SPICS1 on GPIO15 is the PSRAM chip select.

### UDP to a closed port

Sending UDP to a closed port on the gateway makes it answer ICMP
port-unreachable, and the next `sendto` on that socket then fails. It is rare --
once in the several thousand sends of a four-second CSI run, and six deliberate
attempts to provoke it produced nothing -- but it is real.

`common_hal_socketpool_socket_sendto()` reports **any** `lwip_sendto` failure as
`BrokenPipeError`, throwing away the actual errno; `send()` a few lines above
does `mp_raise_OSError(-sent)` and keeps it. Reporting the real error would let
callers tell a transient `ECONNREFUSED` from something that has genuinely gone
wrong. Whether the socket stays unusable afterwards is not established: the run
that hit it recreated the socket, and the failure could not be reproduced on
demand to test a plain retry.

For anything that needs steady traffic, an open port avoids the question.

### BLE

BLE was off on this board with a note claiming NimBLE could not link against the
controller blob because `ble_sm_alg.c` wanted `r_swap_buf` and `r_swap_in_place`,
which the blob did not export. That note was wrong twice over: `nm` shows the C5
blob exporting both, exactly as the C6 one does, and the link was failing on
every symbol in the library, not those two.

The blob simply never reached the link. `ports/espressif/Makefile` keeps a table
of BLE implementations per target -- `BLE_IMPL_esp32c3`, `BLE_IMPL_esp32c6` and
so on -- and adds `libble_app.a` to `BINARY_BLOBS` from it. There was no
`BLE_IMPL_esp32c5` entry, so the variable was empty and the branch that adds the
blob was skipped, while `bt.c` still compiled and asked for its symbols. One line
fixes it. CMake links the blob into `libbt.a` as a private dependency, but this
port drives the final link from its own component list, so the transitive
dependency is not enough on its own.

That leaves size: the firmware with BLE is 2182 kB against the 2048 kB app
partitions both 8 MB layouts use. `partitions-8MB-no-uf2-large-app.csv` gives
larger ones instead. It started at 2304 kB and later went to **2816 kB** to leave
room for the Zigbee stack, which costs about 230 kB on its own; `user_fs` gets
what is left, 2496 kB. The three partitions still add up to exactly the 8128 kB
after otadata, and both app offsets stay on the 64 kB boundary one has to sit on.
The board selects the layout by setting `FLASH_SIZE_SDKCONFIG` in its
`mpconfigboard.mk`; the Makefile's own choice looks only at flash size and
whether there is a UF2 bootloader.

**Changing the partition table moves `user_fs` and CircuitPython reformats it.**
Everything on the board is lost, `settings.toml` included.

Verified on hardware: the adapter comes up as `CIRCUITPY83ee` at
`10:bd:a3:ce:83:ee` and a four-second scan found 13 devices with RSSI,
advertisement payloads and connectable flags.

Coexistence is not free. A 1-165 Wi-Fi scan goes from 9.4 s to 18.6 s, CSI drops
from 108 to 69 records per second, and UDP `sendto` starts failing constantly --
1369 failures in a four-second run, against one in the same run without BLE.
That last one is the mislabelled error described above, now impossible to miss.

### Console input that died and stayed dead

Separate from the ROM hang above, and found the same way. A board would keep
printing perfectly while accepting nothing: every host write ended in a timeout,
and neither closing the port nor a JTAG CPU reset nor reflashing brought it back.
Only pulling power did.

Halted over JTAG the peripheral said it plainly. `EP1_CONF` was `0x06`, so
`SERIAL_OUT_EP_DATA_AVAIL` was set — a byte was waiting in the endpoint FIFO.
`INT_ENA` was `0x104`, so the receive interrupt was enabled. And `INT_RAW` was
`0xf2fb`, with bit 2 clear: nothing pending to trigger it. Reading the FIFO over
JTAG returned a single `0x03`, the ctrl-C that had been sent, and cleared the
flag.

`SERIAL_OUT_RECV_PKT` is raised by the *arrival* of a packet, not by the FIFO
being non-empty, and the two are not simultaneous: the handler can clear the
status and read the FIFO before the byte has surfaced in it. The byte is then
stranded, and because it keeps the endpoint occupied the host cannot send the
next packet either, so no further interrupt can ever arrive. The peripheral is
not reset by a CPU reset, which is why the state survives reflashing.

`usb_serial_jtag_read_char()` would recover it, but only code that reads from the
console calls it — a `code.py` that just computes and prints never does. So
`usb_serial_jtag_rx_tick()` now polls the FIFO from two places: `port_background_task()`,
which runs in the VM loop, and `port_idle_until_interrupt()`, because at the REPL
there is no VM loop to run in and sleeping there waits for a wake-up only the
next keystroke can produce. That last one is what made the console answer one
keystroke behind, every reply belonging to the previous input.

An earlier attempt at this made things worse and is worth recording. Faced with
the stranded byte, `_copy_out_of_fifo()` was changed to always empty the endpoint
and drop whatever the ring buffer could not hold. That keeps the endpoint moving
and silently loses bytes out of the middle of anything longer than the 128-byte
ring buffer — which is every file transfer over the raw REPL, and it presents as
a file that arrives with a `SyntaxError` on line 1. Back-pressure is restored:
what does not fit stays in the FIFO, which is only safe because the poll exists.

One more thing was mixed into this and is not a software fault at all. `INT_RAW`
bits 4-7 are `PID_ERR`, `CRC5_ERR`, `CRC16_ERR` and `STUFF_ERR`, they are sticky,
and on one board all four were set while the other, same firmware, had them
clear. That board's input was corrupting bytes — `0x01` arriving as `0x29`. Moving
it to a different USB port fixed it; after the move neither board accumulates any.
Worth reading those four bits before blaming anything above them.

## 12. Raw 802.15.4 on the ESP32-C5, 2026-09-04

A new `ieee802154` module exposing the radio that sits under Zigbee, Thread and
Matter, as bare frames: what goes out is what the caller hands it, and what comes
in is whatever the channel had on it. It carries no protocol of its own.

The module is ESP-specific, so it lives in `ports/espressif/bindings/ieee802154/`
rather than `shared-bindings`. Building it needs four places touched beyond the
module itself: `CIRCUITPY_IEEE802154` in `py/circuitpy_mpconfig.mk` and
`py/circuitpy_defns.mk`, the source and component lists in the port's `Makefile`,
`CONFIG_IEEE802154_ENABLED` in the board sdkconfig, and -- easy to miss -- the
`COMPONENTS` list in `ports/espressif/CMakeLists.txt`. Without that last one the
component's Kconfig never loads and the sdkconfig option is dropped without a
word. `esp_hal_ieee802154` has to be linked too: it holds `ieee802154_periph`,
the descriptor the driver dereferences on enable.

The whole thing costs about 14 kB.

### What it exposes

Seventeen entry points out of the driver's 42, chosen for what a protocol on top
actually needs: channel, transmit power, PAN ID, short and extended address,
promiscuous and coordinator flags, `send()`, `receive()`, an allocation-free
`readinto()`, `energy_detect()`, the pending-address table for sleepy nodes, and
the acknowledgement controls. Multipan, timed transmit and receive, transmit
security and the enhanced-ACK generator are left out; they belong to a finished
stack rather than a raw radio.

A frame is the MAC header followed by the MAC payload, and nothing else, in both
directions. The PHY length byte and the checksum belong to the radio: the module
writes them into its own buffer on transmit and takes them off on receive, and
neither appears in what Python passes in or gets back.

That is a deliberate change from the driver's own convention, which puts the
length byte in front and has it count the two checksum bytes. Exposing that
directly means the caller has to add two to a length for bytes it never supplies,
and get a buffer back whose last two bytes are not what the length byte claims --
the hardware overwrites the checksum with the RSSI and LQI, which are reported as
fields anyway. Both mistakes are easy to make and neither fails loudly: a frame
with the length byte wrong is simply refused, and one built without room for the
checksum transmits successfully with two bytes of whatever followed the buffer on
the air. Anything the receiver needs to be told, such as how long a payload is,
goes in the payload, where Python puts it and Python reads it back.

Building and parsing the MAC header is deliberately left to Python: it can be
changed without reflashing, and anything built on this needs full control of the
header anyway.

### Acknowledgement, and why there is no manual one

`auto_ack` turns the hardware's automatic acknowledgement on and off at runtime.
There is deliberately no way to send one from Python: the standard allows 192 µs
between the end of a frame and the start of its acknowledgement, which no
interpreter can meet. What Python does control is what the acknowledgement says,
through the pending-address table, and of course any reply sent as an ordinary
frame, which has no deadline at all.

`esp_ieee802154_receive_handle_done()` looks like a candidate for a manual
acknowledgement and is not: it releases the driver's receive buffer.

### Four defects found by measurement

**Coexistence silently vetoes transmission.** All three radios share one 2.4 GHz
front end on this part, and at the driver's default priority the arbiter breaks
802.15.4 transmissions off mid-frame: every `send()` returned
`ESP_IEEE802154_TX_ERR_COEXIST`. Turning Wi-Fi and BLE off from Python does not
help. `esp_ieee802154_set_coex_config()` with `IEEE802154_HIGH` for txrx fixes
it, and the constructor now sets it. Without this the module does not work at
all, and nothing in the error says why.

**The radio cannot be re-enabled.** `esp_ieee802154_enable()` ends in
`ieee802154_mac_init()`, which allocates the interrupt and registers a sleep
callback; `esp_ieee802154_disable()` does not undo that cleanly, so a second
enable returns `ESP_FAIL`. Under CircuitPython that is every second run, since
objects are recreated each time. The driver is now enabled once and left
enabled, and releasing a Radio only puts it to sleep.

**Transmission read the frame out of PSRAM, where DMA cannot see it.** Two boards
transmitted thousands of frames with no errors and neither heard anything at all.
`esp_ieee802154_transmit()` does not copy: it keeps the caller's pointer and
programs it as the radio's DMA source. The frame arrived as a Python `bytes`, and
`port_malloc()` allocates the CircuitPython heap in SPIRAM when there is any --
with a comment right above it saying SPIRAM is not DMA-capable. So the MAC put
whatever it fetched from that address on the air, with a valid checksum computed
over it, and every receiver dropped the frame at the CRC check while the sender
saw a successful transmission.

Nothing anywhere reports this. The driver enables only two of the receive-abort
events, `TX_ACK_TIMEOUT` and `TX_ACK_COEX_BREAK`, so a CRC abort does not even
raise an interrupt. What identified it was a counter incremented on the first
line of the receive callback: it stayed at zero, which separates "the driver
never called us" from "we lost it in our own queue" and pointed below this code
rather than into it. That counter is kept as `Radio.callbacks` — it costs four
bytes and it is the first thing worth reading when frames go missing.

The fix is a static transmit buffer in internal RAM; `send()` blocks until the
radio is done with it, so one is enough. **Any DMA interface in this port handed
a Python buffer has the same exposure on a board with PSRAM.**

**The driver starts promiscuous.** `ieee802154_pib_init()` sets
`promiscuous = true`, so there is no address filtering until someone says
otherwise. The constructor now sets it explicitly, which also turns automatic
acknowledgement on -- that pairing is the driver's, not ours.

Two more were in this code rather than the driver: a `deinit()` that a raised
exception could skip left the radio claimed until the next power cycle, fixed
with a `reset_port()` hook; and `check_esp_err()` collapsed every failure into
`OSError EIO`, which hid the coexistence problem completely. It now reports the
ESP-IDF name and the numeric code, and the numeric part is what identified
`ESP_FAIL` above -- `esp_err_to_name` renders it as "UNKNOWN ERROR".

### Three more, found by writing the examples

Each of these was invisible from the API and only showed up against hardware.

**A channel assigned at run time did not reach the radio.** The driver's setters
only mark the parameter block pending; `ieee802154_pib_update()` applies it, and
that runs inside receive and transmit. So a monitor walking the band stayed on
the channel it started on -- while every getter cheerfully reported the value
that had been written. It cost a channel survey that found nothing, not even the
other board transmitting a metre away. Every setter now re-arms receive through
`esp_ieee802154_receive()`, which is free when nothing is pending.

**`energy_detect()` had the duration in the wrong unit.** The driver counts
symbol periods of 16 us; this passed microseconds. A measurement therefore ran
sixteen times longer than asked, which still returned for a short request and
timed out against this layer's own wait for anything from about 7 ms up. It read
as the radio refusing rather than as a unit error. The range is now checked and
documented in seconds.

**`tx_power` echoed back requests the radio never honoured.** The same
store-now-convert-later split: the driver keeps the number it is given and only
maps it onto the supported set on the way to the hardware, so -40 dBm and 30 dBm
both read back unchanged and the default read 30. The property now snaps a
request to a supported step and stores that, so reading it tells the truth, and
raises for anything outside the range. `tx_powers` exposes the whole set, which
on this part is -24 to 18 dBm in 3 dB steps plus 20.

### Measured

On one board: transmit of a broadcast frame; a frame to an absent node correctly
reported as `Transmit no acknowledgement` rather than a timeout, which shows the
acknowledgement path runs; `energy_detect` returning −104 dBm on a quiet channel;
`promiscuous` toggling and taking `auto_ack` with it; and a Radio created,
released and created again in one run.

On two boards, each transmitting a broadcast twice a second on channel 26 and
listening the rest of the time: every frame the other sent arrived, at −27 to
−29 dBm with LQI 8 to 11, with no transmit errors, nothing dropped by the queue,
and `Radio.callbacks` equal to the number of frames handed to Python — so nothing
is lost between the driver and the caller either.

The frame format was checked from the air rather than from the code. While one
board was still parsing with the old convention it read every field exactly one
byte off and the payload ended cleanly with no trailing bytes, which is what the
new format predicts: one byte removed from the front, two from the back.

The three scripts this was measured with are kept in
`ports/espressif/bindings/ieee802154/examples/`: `tx.py`, `rx.py` and
`monitor.py`, the last a promiscuous capture that decodes the MAC header. Each
is standalone and goes on a board as `code.py`. Their README collects the traps
that cost time here — coexistence, channel choice, and what a monitor cannot see.

## 13. Zigbee on the ESP32-C5, 2026-09-05

A `zigbee` module beside `ieee802154`: the stack rather than the radio. One
build covers coordinator, router and end device, because the role is chosen at
run time and the ZCZR archive's symbols are a strict superset of the end-device
one's. BLE and Wi-Fi stay in the firmware alongside it.

The library is esp-zigbee-lib 2.0.4, vendored under
`ports/espressif/esp-zigbee-lib` as three prebuilt archives plus headers, since
this port builds with `IDF_COMPONENT_MANAGER=0` and cannot fetch components. It
is used through the v2 `ezb_*` API.

### The Trust Center refused every join

The symptom: a coordinator formed a network, opened it, and answered every
joining device with `ZDO Device Update ... tc_action 2`, which is "ignore". A
device was given an address and then never given the network key. Both this
module's own end device and a commercial Sonoff plug were refused, and the same
end device was admitted by a coordinator built from the library's own example --
so the fault was on the coordinator, and in the environment rather than in the
example code.

What it was not, each eliminated by a single-variable test on hardware:
commissioning being driven twice, once from Python and once from the signal
handler; NVS capacity, a shared partition, or a stale half-joined record, tested
with a full erase and with a partition of the stack's own; the size of the APS
key-pair set and of the address, neighbour and route tables, set explicitly
through `ezb_config_memory()` with a successful return; the signal handler
returning false rather than true; internal RAM, retested with 95K free and an
80K largest block, and again with BLE and Wi-Fi compiled out entirely; and the
endpoint's ZHA device type.

The library is a release build and logs nothing on that path, so the chain was
traced one call at a time with `-Wl,--wrap=` on each of `apsme_transport_key_request`,
`aps_process_transmit_security`, `secur_secure_msg`, `zmsg_alloc`,
`crypto_hmac_aes_mmo`, `nwk_address_short_by_extended`, `aps_send_frame`,
`aps_retrans_send_msg` and `nwk_nlde_data_request`. Disassembling
`apsme_update_device_indication` showed that for an unsecured join there are
exactly three paths to "ignore", and the measurements picked out the third: the
Trust Center builds the Transport Key and `apsme_transport_key_request()`
returns an error before anything is transmitted. An 802.15.4 capture of the plug
joining agreed -- association completes, and not one frame follows.

The cause is in the crypto, and not in the Zigbee library at all. Zigbee secures
every frame with AES-CCM\*. In ESP-IDF 6.0 the PSA dispatcher hands an AEAD
request to the ESP hardware driver, which implements GCM and answers
`PSA_ERROR_NOT_SUPPORTED` for anything else; `CONFIG_MBEDTLS_GCM_SUPPORT_NON_AES_CIPHER`
is what lets the dispatcher fall through to the software implementation instead
of returning that error. CircuitPython turns the option off in its shared
`sdkconfig.defaults`. With it off, `secur_secure_msg()` fails for every frame
from the moment the network is formed -- which is also why a formed network
carried almost no data frames at all.

The board's sdkconfig turns it back on. The option's name is misleading:
AES-CCM is neither GCM nor a non-AES cipher, but it takes the same path.

Two notes for anyone repeating this. A wrapper used for tracing has to forward
every argument register: `secur_secure_msg` takes five arguments, and a
four-parameter wrapper made it fail on its own, which looked exactly like the
bug being chased. And the generated `sdkconfig` is treated as a user's
configuration, so changing a defaults file does not move an option that is
already in it -- the file has to be removed for the change to take.

### The module

`zigbee.Stack` brings the stack up on its own task. All of init, endpoint
registration and start happen there, because the library expects the task that
runs the main loop to be the one that set it up; doing it from two tasks leaves
the Trust Center silent. Commissioning is driven from the signal handler, which
returns true as every example the library ships does.

Properties that describe the network are served from a cache refreshed inside
the signal handler, where the stack's lock is already held. Taking that lock for
a property read instead is not an option: a script polling a few times a second
holds the mutex the stack itself needs, and measured that way a device stops
associating at all.

`zigbee.Endpoint` covers every ZHA device type the library can build -- all
eighteen, by their standard device ids, so the number a script builds with is
the number a coordinator reads back from the simple descriptor. Endpoints are
described before `start()` and built by the stack's task in the one window that
allows it. An endpoint's own attributes are read and written by subscript:

    sensor[zigbee.TEMPERATURE_MEASUREMENT, 0x0000] = 2350

Writing goes through `ezb_zcl_set_attr_value()` rather than a raw descriptor
write, because that is the path that tells a configured report or a bound
coordinator that the value changed. `check_access` is false: a sensor writes its
own measured value, and measured values are read-only as seen from the network.

For the other side there are `Stack.read`, `Stack.write`,
`Stack.configure_report`, `Stack.command`, `Stack.discover` and
`Stack.neighbors`. Answers arrive through
`report()` and `descriptor()` rather than being returned, because they arrive
later and on another task. `command()` sends any cluster command by id and
payload rather than wrapping a chosen few.

A read is originated from the client side of a cluster, and the set of clusters
an endpoint may read is exactly its client-role list. Measured across six
endpoints, every one matched its client list character for character: an
`ON_OFF_LIGHT` cannot read even Basic, which it carries as a server, while a
`DIMMER_SWITCH` reads On/Off and Level and nothing else. The failure is
`EZB_ZCL_ERR_NOT_FOUND`, which says a resource was missing but not which one --
the identification is from the measurements, not from the code. `CUSTOM_GATEWAY`
is the exception: it carries fewer clusters than `CONFIGURATION_TOOL` and reads
all of them, being built by a different constructor that marks it as a gateway.
It is therefore what a coordinator wants when the clusters are not known in
advance, and not a general requirement -- an endpoint with the right client
cluster does the job.

`Stack.command` goes through the library's generic command request rather than
its per-cluster ones, and on that path no local cluster check was seen: eight
clusters were accepted from every endpoint tried, including one that could not
read a single one. The per-cluster command functions are documented as checking,
so this is a property of the path the module uses.

What the far end thinks of a command comes back through `report()` as a record
of kind 3, added for this: the command id in `attribute` and the ZCL status.
Measured, a supported command answered 0, an unsupported command id 0x80, and a
command on a cluster the device does not carry 0xC3. Write responses arrive the same way as kind 4:
measured, setting Identify time answered 0 with an id of 0xFFFF meaning all of
them were written, writing the read-only On/Off state answered 0x88, and writing
an attribute that does not exist answered 0x86. None of this makes a returning call proof of transmission or
delivery, and `report()` returning None only means the queue is empty at that
moment -- `lost_reports` counts answers that arrived and were dropped.

`neighbors()` walks the stack's neighbour table directly rather than sending a
Mgmt_Lqi to itself, which is never answered. The table is built from traffic,
not stored: after a reset it starts empty and fills over a few seconds. Only end
devices are children; a router that joined is recorded as a sibling.

### Measured

Two XIAO ESP32-C5 boards, on channel 20. A coordinator formed a network and
admitted an end device: `ZDO Device Update ... tc_action 0`, `ZDO Device
Announce`, `ZDO Device Authorized`. The device kept its address across a reset
and rejoined securely, which it could not do without the network key.

A `TEMPERATURE_SENSOR` built from Python was discovered as device type 0x0302 on
profile 0x0104 with input clusters 0x0402, 0x0003 and 0x0000, and its readings
came back as real values -- 24.11, 24.51, 21.16 degC. Its Basic cluster read back
as manufacturer "CircuitPython" and model "ESP32-C5", so strings and integers
both survive the round trip typed.

A Sonoff mains plug joined the same coordinator and was described as device type
0x0009 with clusters 0x0000, 0x0003, 0x0004, 0x0005, 0x0006 and the
manufacturer's 0xFC57, plus a Touchlink endpoint on 13 and a Green Power one on
242. It was then switched on and off ten times, five seconds apart, with its
state read back after each command and matching every time.

The partition table gained 512K on each OTA partition, to 2816K, which is what
BLE, Wi-Fi and the Zigbee stack need together. The build leaves about 440K of
that free and 195K of internal RAM.

`ieee802154/examples/sniffer.py` takes frames apart layer by layer -- MAC, the
auxiliary security header, Zigbee's NWK, APS and ZCL, or a 6LoWPAN dispatch --
and decrypts the network layer when given a key, with AES-CCM* built from
`aesio`'s ECB mode inside the script. `Stack.network_key` was added for it.
Decryption is verified rather than assumed, which is what distinguishes two
networks sharing a channel: measured with our own network and a Sonoff plug left
on an older one, the first decoded to `NWK command: link status` and the second
was reported as another key's.

The scripts this was measured with are in
`ports/espressif/bindings/zigbee/examples/`: `coordinator.py`, `end_device.py`,
`device.py` (any ZHA type), `reader.py` (discovers and reads everything) and
`outlet.py` (finds a switchable outlet and cycles it). Their README collects
what cost time here.

## 14. Network over USB, 2026-09-11

A `usb_net` module that presents the board to the host as an Ethernet adapter
over USB, the way a small appliance such as siru.box does it: the host gets an
address from the board, and a browser reaches a server on the board by name,
with no other network involved. Built and measured on the M5Stack Cardputer
(ESP32-S3), against Windows 11 and Linux. The ESP32-C5 has no USB device
controller, only USB Serial/JTAG, and cannot do this at all.

The class is CDC-NCM; a build with `CIRCUITPY_USB_NET_RNDIS = 1` uses RNDIS
instead. TinyUSB's two network drivers define the same symbols, so this is a
choice per build, not one `boot.py` can make.

Files: `shared-bindings/usb_net/`, `shared-module/usb_net/` (the class, its
descriptor and the configuration), `ports/espressif/supervisor/usb_net_netif.c`
(esp_netif, DHCP and DNS), and hooks in `supervisor/shared/usb/usb_desc.c`,
`usb.c`, `tusb_config.h`, `supervisor.mk`, `py/circuitpy_mpconfig.mk`,
`py/circuitpy_defns.mk` and the port's `Makefile`.

### Configuration from `boot.py`

```python
import storage, usb_hid, usb_midi, usb_net

storage.disable_usb_drive()
usb_hid.disable()
usb_midi.disable()
usb_net.enable(
    ipv4_address="192.168.7.1",
    netmask="255.255.255.0",
    host_ipv4_address="192.168.7.2",
    gateway=None,
    mac_address=b"\x02\x03\x9d\x5b\x34\xad",
    host_mac_address=b"\x02\x03\x9d\x5b\x34\xac",
    hostname="cardputer.home.arpa",
)
```

Every argument is optional, and the values above are the defaults except the
name, of which there is none. The MAC addresses default to ones derived from
the processor's unique id, and the two must differ. Addresses may be strings
or `ipaddress.IPv4Address`, as with `wifi.radio`. Everything is checked in
`enable()` and raises there, because the IDF's DHCP server replaces a lease
range it does not like with one of its own without saying so.

`CIRCUITPY_USB_NET = 1` compiles the module in; `CIRCUITPY_USB_NET_ENABLED_DEFAULT`
(0) decides whether the interface is presented when `boot.py` says nothing. The
S3 has five IN endpoints counting endpoint 0, and the interface takes two, so
next to the serial console there is no room left for the drive, HID or MIDI:
with any of them on, CircuitPython goes into safe mode after `boot.py`. The
name the host shows for the whole device is its USB product name, which
`supervisor.set_usb_identification()` sets.

### What runs on the board

- An esp_netif on top of the class, with the address from `boot.py`.
- The IDF's DHCP server, with a lease range of exactly one address, since there
  is only ever one host on the link. It offers a router only when `gateway` is
  given. Without that, dhcpcd on Linux added a default route through the
  board.
- A DNS responder. The DHCP server always offers the board as the host's DNS
  server (`dhcpserver.c:495`), so the responder always runs: it answers an A
  query for `hostname`, in any case, with the board's address; any other type
  for that name with no answer and no error; and everything else with REFUSED,
  so the host asks its other servers at once instead of waiting out a timeout.
  TinyUSB's `lib/networking/dnserver.c` was not used because it answers a query
  of any type with an A record and says nothing at all about names it does not
  know. siru.box looks like it runs that very code: the same TTL of 32, no
  answer for other names, and its own name reported twice when Windows asks for
  A and AAAA together.
- Servers in Python use `socketpool.SocketPool(wifi.radio)`: the sockets are
  lwIP's, not wifi's, and a server bound to the board's USB address is reached
  over USB. `adafruit_httpserver` works unchanged.
  `shared-bindings/usb_net/examples/usbnet_demo.py` is a page that shows the
  board's state and sets its NeoPixel from the browser; `import usbnet_demo;
  usbnet_demo.serve()` in code.py is all it takes.

### Three defects found on the way

**Both ends of the link had the same MAC address.** A CDC descriptor's
iMACAddress is the address of the host's end. The board's netif was given the
same one, and the host dropped the board's frames as its own coming back. The
two now differ in the last bit.

**Frames were handed to TinyUSB from the wrong task.** On this port
`tud_task()` runs in a FreeRTOS task of its own. Called from CircuitPython's
task, the class reported every frame sent and put none of them on the wire.

**Then from the right task, but only when `tud_task()` returned.** It waits for
USB events with no timeout and returns after sixteen of them
(`CFG_TUD_TASK_EVENTS_PER_RUN`). With traffic in both directions that comes
round often enough to hide the problem. At the end of an HTTP response it does
not: the host waits for the last frames, sends nothing, and the transfer stops.
Frames are now handed over through `usbd_defer_func()`, which runs a function
inside `tud_task()`, and retried every tick while the class has no room. A host
that takes nothing for 100 retries is not reading the link, and the queue is
dropped, as an Ethernet driver drops frames on a link that is down.

| | before | after |
|---|---|---|
| 64 kB over HTTP | of 3, one stalled at 49 919 bytes until curl gave up at 30 s, the others took 0.40-0.42 s | 13 of 13 complete, over two runs, in 0.28-0.30 s |
| DNS, 1 s timeout | 3 to 4 of 80 queries unanswered | 160 of 160 answered, slowest 8.4 ms |

### Measured

On Linux (WSL 2 over usbipd, kernel 6.18, `cdc_ncm`): the interface enumerates,
the host leases 192.168.7.2 for 7200 s and gets no default route, DNS answers
as described above, ping takes 3.5-6 ms, and HTTP moves about 220 kB/s over
full-speed USB. On Windows 11 the same, with ping at 1 ms. The Cardputer has
no PSRAM, so a large response has to go out in pieces, through
`ChunkedResponse`.

Built with `CIRCUITPY_USB_NET=1 CIRCUITPY_IEEE802154=0 CIRCUITPY_ZIGBEE=0`. The
last two are needed for any S3 build: `mpconfigport.mk` turns 802.15.4 and
Zigbee on for the whole port, and the S3 fails at link without them.

### Windows, and the one line in TinyUSB it needed

Windows 11 (26200) bound its own UsbNcm driver and then refused to start it:
Code 10, status 0xC0000483 (STATUS_DEVICE_FEATURE_NOT_SUPPORTED). The
descriptors read back from the board were correct byte for byte, Linux's
`cdc_ncm` took the same ones, and none of the usual suspects mattered: the
order of the functions, the serial console next to it (network plus drive
alone fails the same way), Windows' cached state (new PID, cleared `UsbFlags`),
a device descriptor with the IAD class 0xEF/0x02/0x01, and TinyUSB's default
NTB sizes.

What settled it was a log of every request as the device saw it, from TinyUSB's
own tracing (`CFG_TUSB_DEBUG 2` with `CFG_TUSB_DEBUG_PRINTF` pointed at a
buffer in RAM, read out over the REPL): the driver read the MAC string,
issued GET_NTB_PARAMETERS, got its 28 bytes, and sent nothing more. An ETW
trace of `USBHUB3` and `UCX` on the host said the same, the failure coming
straight after that reply, so the driver had decided on the parameters alone.
The sample driver Microsoft publishes accepts anything there, but the
UsbNcm.sys that ships (10.0.26100.9444) is not that code: its binary carries
the checks as strings,

```
(m_NtbParameters.wNdpOutDivisor >= 4) && ((m_NtbParameters.wNdpOutDivisor & (m_NtbParameters.wNdpOutDivisor - 1)) == 0) && (m_NtbParameters.wNdpOutDivisor <= m_NtbParameters.dwNtbOutMaxSize)
(m_NtbParameters.wNdpOutAlignment >= 4) && ... power of two ... <= dwNtbOutMaxSize
m_NtbParameters.wNdpOutPayloadRemainder < m_NtbParameters.wNdpOutDivisor
```

and TinyUSB's `ncm_device.c` reports `wNdpOutDivisor = 1`. The divisor is the
modulus the host aligns datagram payloads to in the NTBs it sends; TinyUSB's
parser takes the datagram indices from the NDP and does not care what it is.
Set to `TUD_NCM_ALIGNMENT` (4), Windows starts the adapter: "M5STACK
CircuitPython Network", 12 Mbps, address 192.168.7.2 from the board's DHCP
with no default gateway, `cardputer.home.arpa` resolved by the system resolver,
any other name refused by the board, ping 1 ms, 20 of 20 page fetches by name
in 1.8 s. Linux is unchanged by it. This is a change inside `lib/tinyusb`,
which is a submodule; it belongs upstream.

RNDIS (`CIRCUITPY_USB_NET_RNDIS = 1`) still fails on Windows, with
usbrndis6 and status 0xC0000001, and was not pursued further: NCM is the
protocol every current host supports, and Linux has already dropped
`rndis_host` from the WSL kernel. siru.box, which works on the same machine,
is RNDIS plus mass storage, from an STM32.

## 15. USB audio on the ESP32-S3, 2026-09-11

`usb_audio` (upstream: a UAC2 microphone, speaker or headset over TinyUSB's
audio class) was read through and then run on the Cardputer against Windows
11 26200: descriptors, the class-specific requests, both data paths, and the
DWC2 driver underneath. Four things were wrong, all of them in
`shared-module/usb_audio/__init__.c`.

### The headset's microphone sent nothing

Console, drive and headset enumerated, the host opened the microphone
stream, TinyUSB reported a completed transfer every millisecond, and the host
recorded silence. The headset took two endpoint numbers, `current_endpoint`
for OUT and the next one for IN, so after the console (1 and 2) and the drive
(3) the microphone landed on IN endpoint 5. The ESP32-S3's controller has IN
endpoints 0 to 4 only (`ep_in_count = 5` in TinyUSB's `dwc2_esp32.h`; the
IDF's `usb_dwc_struct.h` has `dieptxf_regs[4]`, one transmit FIFO for each of
IN 1 to 4) while the register block for endpoint 5 exists, so the transfer
"completes" without a FIFO behind it. The count check in `usb_desc.c` passed
because it counts endpoints, not numbers. Both directions now share one number,
as the CDC data and MSC endpoints already do, which also frees a pair.

### The speaker dropped 92 % of what the host sent

`read()` on the speaker, called in a tight loop, took about 1,200 samples a
second from a 16 kHz stream, in chunks of 784 samples: the whole receive ring
(49 ms), roughly one and a half times a second. The USB background task that
moves TinyUSB's OUT FIFO into the ring only ran when something else scheduled
it; on this port TinyUSB is pumped from its own FreeRTOS task, the audio
class handles isochronous completions straight from the interrupt, and the
microphone already covered itself by scheduling the task from
`tud_audio_tx_done_isr()`. The speaker now does the same from
`tud_audio_rx_done_isr()`: 16,000 samples a second reach the ring, a 12,000
amplitude tone from the host reads back as 12,001.

### Windows refused the speaker endpoint

The headset failed to start on Windows with Code 10, status 0xC0440025. The
OUT endpoint was declared asynchronous, and usbaudio2.sys requires an
explicit feedback endpoint for an asynchronous sink, which there is none of.
The board consumes at whatever rate the host sends, so the endpoint is now
adaptive, in the speaker-only descriptor as well; the microphone stays
asynchronous, which needs no feedback. The headset then binds to usbaudio2
without complaint (the speaker alone was not tried on Windows).

### Mute and volume were accepted and ignored

The feature units answered every mute and volume request and stored the
values, and the samples went through untouched. They now apply: per unit
(the headset has one for each direction), master and channel in series, in
Q15 so the sample paths only multiply, mute folded into a zero gain. The
advertised range was -90 to +90 dB; it is -60 to 0 dB in 1 dB steps now, so
the host's slider cannot ask for gain above unity. Measured from Windows:
-20 dB on the microphone takes a 12,000 tone to 1,160 at the host, -20 dB on
the speaker takes the host's 12,000 tone to 1,200 on the board, mute gives
zeros both ways.

Smaller: `enable()` now rejects a `sample_rate` under 1,000, since the
microphone hands the host one millisecond of audio at a time; the module
docstring said a headset used several endpoints per direction and that the
host's controls were ignored; a comment in `tusb_config.h` still described
the module as microphone-only.

### Windows, and the tone that faded

The first symptom on the microphone was not any of the above: a steady 1 kHz
tone reached the host for about 150 ms and then faded to nothing (peaks per
100 ms: 11,774, 4,714, 61, 3, 2, ...), identically through WASAPI and
DirectShow, while the board's counters showed it sending every millisecond.
The endpoint's `FxProperties` in the registry named the reason: an effect
described as "Reduces background noise to help your voice stand out more
clearly by using AI", which learns a stationary tone as noise within a
couple of hundred milliseconds and removes it. A tone pulsed 100 ms on, 100
ms off passes unchanged for as long as the capture runs. So test a USB
microphone on Windows with a signal that is not stationary, or turn the
input's audio enhancements off in Sound settings; the board is not at fault.

### Endpoints on the ESP32-S3

With the headset on one pair: console (IN 1, IN/OUT 2), drive (3) and
headset (4) use every IN endpoint the controller has. A microphone alone
takes the same. Anything else that needs an IN endpoint -- HID, MIDI, the
network -- means turning the console or the drive off in `boot.py`.

### The other four classes, checked the same day

CDC (console and a second port), MSC, HID and MIDI were read through and
run on the Cardputer against Windows 11: `usb_cdc.data` read 100 bytes
trickled one at a time and wrote 3000 complete; a file written from Windows
read back on the board; a Scroll Lock press from `usb_hid` came back as the
host's LED report (0x05, then 0x01); three MIDI messages sent from winmm
came back through `usb_midi` byte for byte. Three things in the code, none
of which showed in the tests: the loops in `shared-module/usb_cdc/Serial.c`
compare the running total with the *remaining* length, so the driver alone
returns after about half of a request -- `py/stream.c` calls it again until
the count is met, and the only trace is that the timeout restarts after each
partial chunk; `usb_desc.c` checks the string table index with `>` where the
table has 16 slots numbered from 0, unreachable on this chip; and
`usb_msc_flash.c` refreshes FatFs's cached sector only when it is the first
block of a host write, while the S3 takes eight blocks per callback.

## 16. A USBTMC instrument, 2026-09-11

`usb_tmc` presents the board as a USB Test and Measurement Class device,
USB488 subclass, the class VISA talks to: `usb_tmc.enable()` in `boot.py`,
then `usb_tmc.read()` hands Python each command message the host sends and
`usb_tmc.write()` queues the response for the host's next read. The class
is message-based -- every transfer carries a header with the message's
length and an end-of-message bit -- so the API is too; a stream would throw
the boundaries away and then have to guess them back. `status_byte()` and
`set_status_byte()` are the IEEE 488.2 status byte, with the message
available bit kept by the module while a response is pending.

One interface, one endpoint pair, no interrupt endpoint (which would cost
the second IN endpoint console + drive + instrument does not have), so no
service requests and `is488_2 = 0`; the capabilities say SCPI. Commands
queue in a 1 kB ring behind two-byte lengths; the bulk OUT endpoint is left
NAKing when the ring is nearly full, so a slow `code.py` makes the host wait
rather than lose a command. A command that could never fit is refused, which
halts the endpoint and the host reports an error. The response stays in its
own buffer until TinyUSB has sent it, in as many bulk IN requests as the host
takes to collect it. A request for a response that Python has not written
yet is remembered and answered when it is, the NAK the specification asks
for.

Measured from WSL (pyvisa 1.16.2, pyvisa-py 0.8.1 over pyusb, the board
attached with usbipd): `*IDN?` answered in 3 ms, a 751-character response
collected across packets intact, an unanswered query timing out on the host
and the next query working (the abort sequence), `READ_STATUS_BYTE` giving
0x30 with a response pending and 0x20 after it was read, `INITIATE_CLEAR`
dropping a pending response, and 20 queries in 0.12 s. pyvisa-py itself has
no `read_stb()` or `clear()` for USB; both were done as raw control
requests. The WSL kernel has no `usbtmc` module; pyusb is what works there.

Windows ships no USBTMC driver: the interface shows with no driver until
WinUSB is bound to it (Zadig) for pyvisa-py, or NI-VISA brings its own.
With WinUSB bound, the same script gives the same results on Windows 11
(`*IDN?` in 2 ms, 20 queries in 0.09 s), with one trap on this machine:
pyusb found a stray libusb0 backend, which cannot see WinUSB devices, and
only falls back to `libusb-package`'s libusb-1.0 when it finds no backend
at all; putting that DLL's directory first on `PATH` makes libusb1 win.

### Examples

`shared-bindings/usb_tmc/examples/usbtmc_demo.py` is a small SCPI
instrument for `code.py`: `*IDN?`, `*RST`, `*CLS`, `*ESR?`, `*ESE`, `*OPC?`,
the chip temperature and uptime as measurements, an output on the board LED
where there is one, and an error queue behind `SYST:ERR?` in the SCPI
convention (`-113,"Undefined header"`), with the status byte's event-summary
and error-queue bits kept current. It polls with `read(timeout=0)` -- `read`
blocks only when asked to; `timeout=0` and `in_waiting()` are the
non-blocking ways in -- so `serve(idle=...)` gives the rest of `code.py` its
turn between messages.

`visa_client.py` next to it runs through all of that from a PC with pyvisa
and pyvisa-py, on Windows with the libusb path fix above. One thing it
shows that the module cannot hide: a VISA `write` returns as soon as the
message is delivered, before Python on the board has run it, so a status
byte read straight after sees the old state; `*OPC?` first, as instruments
expect, and the status is current (0x24 after an unknown command with the
event enable mask set). Fifty queries take 0.13 s.

## 17. Upstream merge, 2026-09-12

`adafruit/main` at `42af95d52e` merged, moving the base from `10.3.0-rc.0`
to `10.4.0-alpha.1`. 1,581 commits since the last merge, 1,484 of them not
merges, the bulk being **MicroPython v1.28** (t-strings, `weakref`, the
list and tuple helpers moved into `objlist.h`/`objtuple.h` as inlines,
`mp_handle_pending` taking an enum, `mp_obj_fun_get_name` renamed), plus
the loader-only native code support behind `CIRCUITPY_LOAD_NATIVE` that
section 6 could not reach (the emitter stays off; see the note on Turbo
below), an `emmcio` module, and Adafruit's own ESP32-C5 port and 5 GHz
scanning, which ran into this fork's from section 11. ESP-IDF stays at
`v6.0.1-7-g2b900d1222`; the TinyUSB submodule is untouched, so the fork's
`peterbay/tinyusb` pointer with the NCM divisor fix survives.

### Conflicts, 17 files

Small ones: `objfun.h` and `objtype.h` keep both sides; `objtuple.c` loses
the fork's `mp_obj_tuple_get` (now an inline in the header) and
`mp_obj_tuple_del` (removed upstream, `objzip.c` adjusted with it);
`usb_audio` keeps ours, since upstream made the same ADAPTIVE change and
only a comment differed; `usb_msc_flash.c` numbers the LUNs for both the
`emmcio` disk and section 3's partition disk; `Processor.c`, the C5 pin
tables, `build_memory_info.py` and the Makefile differed in comments and
ordering only.

The ones that took a decision: the ESP32-C5 pin-reset list keeps ours,
which protects the PSRAM chip select and the USB pins unconditionally
(section 11 shows what dropping the flash chip select does), with
upstream's finding that `gpio_ll_func_sel()` on the USB pins clears
`usb_pad_enable` folded into the comment. The scan pattern keeps ours --
DFS channels included, non-DFS first, a channel the country setting refuses
skipped rather than ending the scan -- under upstream's `SOC_WIFI_SUPPORT_5G`
guard. `sdkconfig-esp32c5.defaults` takes upstream's NimBLE block, since the
fork's note claiming the blob could not be linked was already known wrong
(section 11), and keeps `CONFIG_ESP_PHY_ENABLE_USB`. The C5 block of
`mpconfigport.mk` takes `CIRCUITPY_CANIO = 0` (TWAI-FD needs a new backend,
which section 11 lists as open) and `CIRCUITPY_AUDIOBUSIO_PDMIN = 0` from
upstream and keeps `CIRCUITPY_BLEIO_NATIVE ?= 1`. `Terminal.c` had both
sides fixing the same escape-parser overrun differently; the fork's
`TERM_PEEK`/`TERM_ADVANCE` version stays, and upstream's bound on the
UTF-8 continuation-byte scan closes the hole the fork's comment had
admitted to. `WaveFile.c` takes upstream's simpler buffer length now that
the binding checks for a multiple of 8.

### Three things that were not conflicts

An untracked copy of `main.c` was sitting in `ports/espressif/` (dated
2026-09-08, identical to the pre-merge top-level file), and the port's
build takes `main.c` from its own directory first, so the first build
after the merge failed on `PYEXEC_EXCEPTION`, a name upstream had just
removed -- in a file git said it had merged cleanly. It is moved out of the
tree (`~/zbwork/stray_ports_espressif_main.c`).

With the `custom` module's `.mk` hunks stashed for the merge, the build
directories still held that module's `genhdr` registration split files, so
the link failed on `custom_module`. Deleting the stale `genhdr` artefacts
and the module's objects from the build directory is enough; a fresh build
directory is not needed.

`ports/unix` no longer builds its `standard` variant: `py/objringio.c`
(MicroPython's `micropython.RingIO`) is now compiled in and wants
MicroPython's `ringbuf` API, which CircuitPython's `py/ringbuf.h` does not
have. The trees do not differ there, so it is upstream's breakage; their
`coverage` variant -- now the Makefile's default -- turns `RINGIO` off and
builds. Section 9's numbers were from `standard`; the suite below is
`coverage`. `run-tests.py` also lost `--pyboard-device`; it is
`-t unix` with `MICROPY_MICROPYTHON` pointing at the binary.

### Verification

| | |
| --- | --- |
| Cardputer, `CIRCUITPY_USB_NET=1 CIRCUITPY_USB_TMC=1` | builds, 3,957,760 B UF2 |
| Seeed XIAO ESP32-C5 | builds, 268,848 B of 384 kB HP SRAM used |
| MicroPython's suite against `ports/unix` (`coverage`) | 972 tests, 30,721 cases, none failed (section 9: 934 / 27,609, the growth being v1.28's tests) |
| Regression tests on the Cardputer, merged firmware | 220 cases across six files, no failures: `delattr_test` 24, `negidx_test` 53, `subprop_test` 20, `digitalio_test` 25, `readline_test` 48, `gcstress_test` 50 |
| USB on the Cardputer | console, drive and the USBTMC instrument enumerate; section 16's VISA client runs through, 50 queries in 0.12 s |

Not run on hardware: the XIAO ESP32-C5 (built only), the USB network and
audio configurations (unchanged by the merge apart from the resolved
comment), and the `custom` module, whose `.mk` hunks were stashed for the
merge. That module calls `mp_obj_list_get()`, which is now an inline in
`py/objlist.h` with the `obj.h` declaration gone, so its source gets that
include with the stash.

### Turbo, for the record

The loader that arrived here is what the "CircuitPython Turbo" guide
builds on: `mpy-cross` on the host emits native `.mpy` for the board's
architecture and the board only loads them. Section 6 found the S3 had no
executable memory for code the board emits itself; whether the loader path
solves placement on the S3 (the Turbo project reports 26x on a Metro
ESP32-S3) is a separate investigation, not part of this merge.

## 18. TrueType glyphs from the outline, 2026-09-12

`ttfrast` renders glyphs straight from a `.ttf` at any size with anti-aliased
edges, so one font file serves every text size. It is off by default and
built with `CIRCUITPY_TTFRAST=1`; it adds 6.5 kB on the Cardputer and needs
`displayio`.

The question it answers is what vector text costs on this hardware, so the
algorithm is the fastest one known for CPUs: the signed-area accumulation of
Raph Levien's font-rs (fontdue is a fork of it, stb_truetype v2 does the
same thing). Every outline segment writes coverage *deltas* -- exact
trapezoid areas -- into a dense float grid, and one linear pass with a
running sum turns the grid into 8-bit coverage. There is no edge sorting and
no active edge list. Quadratic curves are flattened by the deflection of the
control point and stepped by forward differencing. The parser reads `head`,
`maxp`, `loca`, `glyf`, `hhea`, `hmtx` and a format 4 `cmap`, composites
included; CFF outlines are refused.

`Font(data)` keeps a reference to the bytes and copies nothing. `render()`
gives raw coverage and the glyph geometry, `render_into()` draws with the
pen on a baseline into any `displayio.Bitmap` up to 8 bits deep, quantizing
to the bitmap's depth and leaving untouched pixels alone. A font created
with `size=` follows the `fontio` protocol: glyphs are rendered on first use
into an atlas bitmap of uniform cells -- the font's bounding box at that
size, taken from the glyphs present rather than the `head` table, which in a
subset font still spans the original -- and `get_glyph()` returns a
`fontio.Glyph` whose tile index addresses the cell, so
`adafruit_display_text.label` and `displayio.TileGrid` take it like a
`BuiltinFont`. Slots are reused round robin, so `max_glyphs` must cover the
distinct characters on screen. `bits_per_pixel=4` gives anti-aliased cells;
`label` keeps a two-entry palette, so its tile grids get a 16-step ramp
palette assigned afterwards.

### Measured

Cardputer, Verdana, mean over the glyphs of "Hamburgefonstiv", timed inside
C (`time_render`) so the interpreter is not in the number:

| size | µs per glyph | of it parse + draw | of it accumulate | µs per pixel |
| --- | --- | --- | --- | --- |
| 12 px | 76 | 69 | 8 | 0.99 |
| 16 px | 85 | 75 | 10 | 0.70 |
| 24 px | 105 | 88 | 17 | 0.45 |
| 32 px | 133 | 101 | 31 | 0.35 |
| 48 px | 195 | 131 | 64 | 0.25 |
| 64 px | 265 | 151 | 114 | 0.20 |

The cost grows with the outline's perimeter, not the area: 64 px is twice
16 px for eleven times the pixels. A glyph with no outline costs 1.2 µs, so
the table lookups are nothing; the fixed part is the segment count (2 to
3 µs per segment for the scanline setup). Section 5's `vectorio` polygon at
1.2 + 0.14 µs per edge and pixel, and the 1 bpp blit at 0.54 µs per pixel,
are both dearer per pixel at 24 px and above, though they also composite to
the display, which this does not.

The first version was 70 to 80 % slower: `floorf`, `ceilf`, `fminf` and
`fmaxf` are library calls on this toolchain and the scanline loop ran them
twice a row. Replacing them with casts and comparisons was the whole gain.
Forward differencing for the curves, tried afterwards, changed nothing
measurable, which says where the time is not.

Drawing "Ahoj" into the 240x135 display bitmap: 59 ms at 64 px with the
coverage copied by a Python loop, 4 ms through `render_into`. A ten-glyph
`label` builds in 19 ms at 1 bpp and 28 ms at 4 bpp, most of that the
library's own `TileGrid` work.

The accumulation grid is 4 bytes per pixel, so a 128 px glyph needs 21 kB;
with the 135 kB heap this board's GC gets (the PSRAM is not in it), the
ceiling is around 180 px. Fixed point would halve that. The same heap is
why a full Verdana (243 kB) does not load and the measurements used an
ASCII subset made with `fontTools.subset`; outlines are unchanged by
subsetting.

### Second pass, from a review

A written review of the module (`TTFRAST-OPTIMALIZACE.md`) made seven
proposals; two had weight and were done, the rest were small against the
measurements or need a profile first.

A `get_glyph` hit used to run the whole rasterizer again just to read the
advance; the advance is now kept per slot and a hit is the slot search and
the `Glyph` tuple, 12.5 µs against a full render. The blit into a bitmap
went through `displayio_bitmap_write_pixel`, which rechecks read-only,
bounds and format per pixel; it now packs rows directly at the bitmap's
depth, with the byte held across the pixels it covers, and the atlas cell
is cleared the same way. That also fixed a quantization slip: a pixel whose
coverage rounded to 0 was still written, punching holes into whatever was
under the text at 2 and 4 bpp. A ten-glyph 4 bpp `label` builds in 17 ms
(was 28), "Ahoj" at 64 px into the display bitmap takes 3.4 ms (was 4.0).

The review also pointed out that `render(..., None)` still draws the
outline into the grid and skips only the accumulation, which the docstrings
had called "stopping after the outline"; the wording is fixed here and in
the bindings, and the table above says "parse + draw".

### `ttfrast.Label`, the label in C

The measurements above left the Python `label` as the biggest item on the
path -- 17 to 35 ms for ten glyphs against 1 ms of rasterization -- so the
layout moved into C. `ttfrast.Label(font, text, size=, color=,
background_color=, line_spacing=, bits_per_pixel=, scale=, x=, y=)` lays
the string out from the outline metrics, sizes one bitmap to the ink,
renders every glyph into it through `render_into` and shows it in one
`TileGrid` behind a palette that ramps from the background colour (or
black, when transparent) to the text colour. Newlines start a new line at
`line_spacing` ems. `text` relayouts, keeping the bitmap when the extent
did not change; `color` and `background_color` only rewrite the palette;
`bounding_box` gives the ink relative to the origin, which is the pen at
the start of the first baseline; `anchor_point` and `anchored_position`
move the group so a chosen point of the box lands where asked.

It is a `displayio.Group`, and by a route worth writing down: displayio
accepts subclasses of `Group` by casting to the native base the way it
does for Python subclasses -- reading `subobj[0]` of an
`mp_obj_instance_t` -- so the object keeps that layout (base, an empty
members map, `subobj[0]` holding the real Group), pinned by a static
assert. Group's methods and properties resolve through the type's
`parent` and act on that inner Group; type slots are not inherited, so
`len()`, indexing and iteration are forwarded by hand, and attribute
stores look only in the type's own dict, so Group's settable properties
(`x`, `y`, `scale`, `hidden`) are listed in the Label's dict again.

Measured on the Cardputer, "Česká věta" at 26 px, 4 bpp: the C label
builds in 3.9 ms and 9.7 kB of heap; the Python label with the same font
took 21 to 35 ms across runs. Setting new text costs 3.4 ms whether or not
the bitmap is reused, which says the time is the rasterization, not the
allocation.

The label's layout pass used to call `render(..., NULL)` for every
character, which draws the outline into the grid and skips only the
accumulation, so each glyph was rasterized 1.7 times. `metrics()` reads
the box from the glyf header and the advance from hmtx without touching
the outline; the layout pass calls that, and it is exposed to Python as
`Font.metrics()` for the same reason. Setting the label's text went from
3.4 ms to 2.4 ms for ten glyphs at 26 px.
