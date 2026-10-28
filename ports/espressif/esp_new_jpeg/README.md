# esp_new_jpeg

Espressif's JPEG encoder, used by `usb_video` to stream MJPEG. Version **1.0.2**,
taken from
[esp-adf-libs](https://github.com/espressif/esp-adf-libs/tree/master/esp_new_jpeg).

Only what the build uses is kept here: the headers, the licence, and one
prebuilt archive per target under `lib/`. The component's own `CMakeLists.txt`
and `idf_component.yml` are not included, because this is not registered as an
IDF component -- `ports/espressif/Makefile` adds the headers to `CFLAGS` and the
archive to `BINARY_BLOBS` directly, so a board that does not stream MJPEG pays
nothing.

The archives are committed past the `*.a` rule in `.gitignore`, so adding a
target means `git add -f ports/espressif/esp_new_jpeg/lib/<target>/`.

## Why the version is pinned here rather than fetched

This encoder ignores the output buffer size it is given and writes past the end
of the buffer instead of reporting that a frame did not fit. `usb_video` works
around that by sizing its buffers for the worst case it measured against *this*
build of the library, so the exact bytes matter -- see
`CIRCUITPY_USB_VIDEO_JPEG_FRAME_BYTES` in `supervisor/supervisor.mk`.
