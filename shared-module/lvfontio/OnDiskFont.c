// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "shared-bindings/lvfontio/OnDiskFont.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/stream.h"
#include "py/objstr.h"
#include "py/gc.h"
#include "shared-bindings/displayio/Bitmap.h"
#include "extmod/vfs_fat.h"
#include "lib/oofatfs/ff.h"
#include "supervisor/shared/translate/translate.h"
#include "supervisor/port.h"
#include "supervisor/shared/serial.h"
#include "supervisor/filesystem.h"

// Helper functions for memory allocation
static inline void *allocate_memory(lvfontio_ondiskfont_t *self, size_t size) {
    void *ptr;
    if (self->use_gc_allocator) {
        ptr = m_malloc_maybe(size);
    } else {
        ptr = port_malloc(size, false);
    }
    if (ptr != NULL) {
        return ptr;
    }
    common_hal_lvfontio_ondiskfont_deinit(self);
    if (self->use_gc_allocator) {
        m_malloc_fail(size);
    }
    return NULL;
}

static inline void free_memory(lvfontio_ondiskfont_t *self, void *ptr) {
    if (self->use_gc_allocator) {
        m_free(ptr);
    } else {
        port_free(ptr);
    }
}

// Forward declarations for helper functions
static int16_t find_codepoint_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint);
static bool slot_has_active_full_width_partner(lvfontio_ondiskfont_t *self, uint16_t slot);
static void invalidate_full_width_partner(lvfontio_ondiskfont_t *self, uint16_t slot);
static uint16_t find_free_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint, uint16_t slots_needed);
static FRESULT read_bits(FIL *file, size_t num_bits, uint8_t *byte_val, uint8_t *remaining_bits, uint32_t *result);
static FRESULT read_glyph_dimensions(FIL *file, lvfontio_ondiskfont_t *self, uint32_t *advance_width, int32_t *bbox_x, int32_t *bbox_y, uint32_t *bbox_w, uint32_t *bbox_h, uint8_t *byte_val, uint8_t *remaining_bits);

// Load font header data from file
static bool load_font_header(lvfontio_ondiskfont_t *self, FIL *file, size_t *max_slots) {
    UINT bytes_read;
    FRESULT res;

    // Start at the beginning of the file
    res = f_lseek(file, 0);
    if (res != FR_OK) {
        return false;
    }

    uint8_t buffer[8];
    bool found_head = false;
    bool found_cmap = false;
    bool found_loca = false;
    bool found_glyf = false;


    size_t current_position = 0;

    // Read sections until we find all the sections we need or reach end of file
    while (true) {
        // Read section size (4 bytes)
        res = f_read(file, buffer, 4, &bytes_read);
        if (res != FR_OK || bytes_read < 4) {
            break; // Read error or end of file
        }

        uint32_t section_size = buffer[0] | (buffer[1] << 8) |
            (buffer[2] << 16) | (buffer[3] << 24);

        if (section_size == 0) {
            break; // End of sections marker
        }

        // Read section marker (4 bytes)
        res = f_read(file, buffer, 4, &bytes_read);
        if (res != FR_OK || bytes_read < 4) {
            break; // Read error or unexpected end of file
        }


        // Make a null-terminated copy of the section marker for debug printing
        char section_marker[5] = {0};
        memcpy(section_marker, buffer, 4);

        // Process different section types
        if (memcmp(buffer, "head", 4) == 0) {
            // Read head section data (35 bytes)
            uint8_t head_buf[35];
            res = f_read(file, head_buf, 35, &bytes_read);
            if (res != FR_OK || bytes_read < 35) {
                break;
            }

            // Skip version (4 bytes) and padding (1 byte)
            // Parse font metrics at offset 6
            self->header.font_size = head_buf[6] | (head_buf[7] << 8);
            self->header.ascent = head_buf[8] | (head_buf[9] << 8);
            self->header.default_advance_width = head_buf[22] | (head_buf[23] << 8);

            // Parse format information
            self->header.index_to_loc_format = head_buf[26];
            self->header.bits_per_pixel = head_buf[29];
            // CIRCUITPY-CHANGE: the format allows 1 to 4 and the field was never
            // checked. Anything larger fed "1 << bits_per_pixel" below, and a value
            // near 32 makes that shift undefined as well as producing a nonsense
            // cache size.
            if (self->header.bits_per_pixel < 1 || self->header.bits_per_pixel > 4) {
                return false;
            }
            self->header.glyph_bbox_xy_bits = head_buf[30];
            self->header.glyph_bbox_wh_bits = head_buf[31];
            self->header.glyph_advance_bits = head_buf[32];

            // Calculate derived values
            self->header.glyph_header_bits = self->header.glyph_advance_bits +
                2 * self->header.glyph_bbox_xy_bits +
                2 * self->header.glyph_bbox_wh_bits;
            self->header.glyph_header_bytes = (self->header.glyph_header_bits + 7) / 8;

            found_head = true;
        } else if (memcmp(buffer, "cmap", 4) == 0) {
            // Read subtable count
            uint8_t cmap_header[4];
            res = f_read(file, cmap_header, 4, &bytes_read);
            if (res != FR_OK || bytes_read < 4) {
                break;
            }

            uint32_t subtable_count = cmap_header[0] | (cmap_header[1] << 8) |
                (cmap_header[2] << 16) | (cmap_header[3] << 24);

            // Allocate memory for cmap ranges
            self->cmap_range_count = subtable_count;
            self->cmap_ranges = allocate_memory(self, sizeof(lvfontio_cmap_range_t) * subtable_count);
            if (self->cmap_ranges == NULL) {
                return false;
            }

            // Read each subtable
            for (uint16_t i = 0; i < subtable_count; i++) {
                uint8_t subtable_buf[16];
                res = f_read(file, subtable_buf, 16, &bytes_read);
                if (res != FR_OK || bytes_read < 16) {
                    break;
                }

                // Read data_offset (4 bytes)
                uint32_t data_offset = subtable_buf[0] | (subtable_buf[1] << 8) |
                    (subtable_buf[2] << 16) | (subtable_buf[3] << 24);

                // Read range_start, range_length, glyph_offset
                uint32_t range_start = subtable_buf[4] | (subtable_buf[5] << 8) |
                    (subtable_buf[6] << 16) | (subtable_buf[7] << 24);
                uint16_t range_length = subtable_buf[8] | (subtable_buf[9] << 8);
                uint16_t glyph_offset = subtable_buf[10] | (subtable_buf[11] << 8);
                uint16_t entries_count = subtable_buf[12] | (subtable_buf[13] << 8);

                // Get format type (0=sparse mapping, 1=range mapping, 2=range to range, 3=direct mapping)
                uint8_t format_type = subtable_buf[14];
                // Check for supported format types (0, 2, and 3)
                if (format_type != 0 && format_type != 2 && format_type != 3) {
                    continue;
                }

                // Store the range information
                self->cmap_ranges[i].range_start = range_start;
                self->cmap_ranges[i].range_end = range_start + range_length;
                self->cmap_ranges[i].glyph_offset = glyph_offset;
                self->cmap_ranges[i].format_type = format_type;
                self->cmap_ranges[i].data_offset = current_position + data_offset;
                self->cmap_ranges[i].entries_count = entries_count;
            }

            found_cmap = true;
        } else if (memcmp(buffer, "loca", 4) == 0) {
            // Read max_cid
            uint8_t loca_header[4];
            res = f_read(file, loca_header, 4, &bytes_read);
            if (res != FR_OK || bytes_read < 4) {
                break;
            }

            // Store max_cid value
            self->max_cid = loca_header[0] | (loca_header[1] << 8) |
                (loca_header[2] << 16) | (loca_header[3] << 24);

            // Store location of the loca table offset data
            self->loca_table_offset = current_position + 12;

            found_loca = true;
        } else if (memcmp(buffer, "glyf", 4) == 0) {
            // Store start of glyf table
            self->glyf_table_offset = current_position;
            size_t advances[2] = {0, 0};
            size_t advance_count[2] = {0, 0};
            // CIRCUITPY-CHANGE: the two buckets latch onto the first two distinct advance
            // widths in glyph order and never move, so they describe nothing about a font
            // that has more than two. The widest advance actually seen is what the cell has
            // to hold in that case.
            size_t max_advance = 0;
            // Glyphs whose advance width matches neither bucket. Counted as full width
            // below because that is the worst case for the slot count.
            size_t other_count = 0;
            bool census_complete = true;

            if (self->header.default_advance_width != 0) {
                advances[0] = self->header.default_advance_width;
            }

            // Set the default advance width based on the first character in the
            // file.
            size_t cid = 0;
            // CIRCUITPY-CHANGE: max_cid comes out of the file and the old bound
            // underflowed to SIZE_MAX for max_cid == 0. The FR_OK checks below do not
            // bound it either: read_bits returns FR_OK without touching the file when
            // it is asked for zero bits, so a header declaring zero-width glyph fields
            // spins here doing no I/O at all. The max_glyphs term is an early exit
            // rather than a safety one -- max_slots is only ever used as a MIN against
            // max_glyphs and every glyph contributes at least one slot, so once that
            // many have been counted the answer cannot change, and the rest of the
            // table costs one f_read per byte, twice per boot.
            while (cid + 1 < self->max_cid && cid < UINT16_MAX && cid < self->max_glyphs) {
                // Read glyph header fields
                uint32_t glyph_advance;
                int32_t bbox_x, bbox_y;
                uint32_t bbox_w, bbox_h;

                uint8_t byte_val = 0;
                uint8_t remaining_bits = 0;

                // CIRCUITPY-CHANGE: neither read below was checked. Past the end of the
                // glyph data read_glyph_dimensions returns before assigning
                // glyph_advance and then keeps failing, so the census went on counting
                // one uninitialized value; and max_cid comes from the file, so a max_cid
                // of 0 made the condition above cid < SIZE_MAX and the loop ran until the
                // whole rest of the file had been read a byte at a time.
                // Use the helper function to read glyph dimensions
                res = read_glyph_dimensions(file, self, &glyph_advance, &bbox_x, &bbox_y, &bbox_w, &bbox_h, &byte_val, &remaining_bits);
                if (res != FR_OK) {
                    census_complete = false;
                    break;
                }

                // Throw away the bitmap bits.
                res = read_bits(file, self->header.bits_per_pixel * bbox_w * bbox_h, &byte_val, &remaining_bits, NULL);
                if (res != FR_OK) {
                    census_complete = false;
                    break;
                }

                if (glyph_advance > max_advance) {
                    max_advance = glyph_advance;
                }

                if (glyph_advance == 0) {
                    // Ignore zero-advance glyphs when inferring the terminal cell width.
                    // Some fonts include placeholders/control glyphs with zero advance,
                    // which would otherwise skew default_advance_width too small.
                } else if (advances[0] == glyph_advance) {
                    advance_count[0]++;
                } else if (advances[1] == glyph_advance) {
                    advance_count[1]++;
                } else if (advance_count[0] == 0) {
                    advances[0] = glyph_advance;
                    advance_count[0] = 1;
                } else if (advance_count[1] == 0) {
                    advances[1] = glyph_advance;
                    advance_count[1] = 1;
                } else {
                    // CIRCUITPY-CHANGE: a third distinct advance width abandoned the
                    // census here. max_glyphs is capped to what it reports, so a font with
                    // mixed advance widths sized the whole glyph cache from the handful of
                    // glyphs seen before that third width appeared and the terminal left
                    // every cell that no longer fit blank.
                    other_count++;
                }
                cid++;
            }

            if (self->header.default_advance_width == 0) {
                // CIRCUITPY-CHANGE: halving the wider bucket recovers the cell of a font that
                // has exactly two advance widths, one about twice the other. It was applied to
                // any font with more than one, and the buckets hold whichever two advances
                // appeared first, so a proportional font got half of an arbitrary early glyph:
                // Arial at size 16 runs from 3 to 16 px and this produced 3. cache_glyph then
                // called nearly every glyph full width, and two cells of 3 px do not hold a
                // 16 px glyph either -- load_glyph_bitmap clips against the whole bitmap, not
                // against the slot, so each glyph overwrote the ones cached after it. A third
                // distinct advance means the font is not dual width and no half-width cell can
                // be inferred, so use the widest advance the census saw: the narrowest cell
                // that holds every glyph on its own.
                if (advance_count[1] == 0) {
                    self->header.default_advance_width = advances[0];
                } else if (other_count != 0) {
                    self->header.default_advance_width = max_advance;
                } else if (advances[0] > advances[1]) {
                    self->header.default_advance_width = advances[0] / 2;
                } else {
                    self->header.default_advance_width = advances[1] / 2;
                }
            }

            if (self->header.default_advance_width == 0) {
                self->header.default_advance_width = 1;
            }

            // CIRCUITPY-CHANGE: the slot count charged one slot per glyph to every bucket
            // except the one the inference had picked as full width, but cache_glyph gives
            // two slots to any glyph wider than the cell. Under-counting here caps
            // max_glyphs below the number of cells the terminal has and those cells stay
            // blank, so the count now follows the same rule the cache does.
            *max_slots = other_count * 2;
            for (size_t i = 0; i < 2; i++) {
                *max_slots += advance_count[i] * (advances[i] > self->header.default_advance_width ? 2 : 1);
            }
            if (!census_complete) {
                // Nothing else bounds the cache, so a census that stopped early must not
                // be allowed to shrink it.
                *max_slots = UINT16_MAX;
            }
            if (*max_slots == 0) {
                *max_slots = 1;
            }
            found_glyf = true;
        }

        current_position += section_size;

        // Skip to the end of the section
        res = f_lseek(file, current_position);
        if (res != FR_OK) {
            break;
        }

        // If we found all needed sections, we can stop
        if (found_head && found_cmap && found_loca && found_glyf) {
            break;
        }
    }

    // Check if we found all required sections
    if (!found_head || !found_cmap || !found_loca || !found_glyf) {
        return false;
    }

    return true;
}

// Get character ID (glyph index) for a codepoint
static int32_t get_char_id(lvfontio_ondiskfont_t *self, uint32_t codepoint) {
    // Find codepoint in cmap ranges
    for (uint16_t i = 0; i < self->cmap_range_count; i++) {
        // Check if codepoint is in range for this subtable
        if (codepoint >= self->cmap_ranges[i].range_start &&
            codepoint < self->cmap_ranges[i].range_end) {

            // Handle according to format type
            switch (self->cmap_ranges[i].format_type) {
                case 0: { // Sparse mapping - need to look up in a sparse table
                    if (!self->file_is_open) {
                        return -1;
                    }

                    // Calculate the relative position within the range
                    uint32_t idx = codepoint - self->cmap_ranges[i].range_start;

                    if (idx >= self->cmap_ranges[i].entries_count) {
                        return -1;
                    }

                    // Calculate the absolute data position in the file
                    uint32_t data_pos = self->cmap_ranges[i].data_offset + idx; // 1 byte per entry
                    FRESULT res = f_lseek(&self->file, data_pos);
                    if (res != FR_OK) {
                        return -1;
                    }

                    // Read the glyph ID (1 byte)
                    uint8_t glyph_id;
                    UINT bytes_read;
                    res = f_read(&self->file, &glyph_id, 1, &bytes_read);

                    if (res != FR_OK || bytes_read < 1) {
                        return -1;
                    }


                    return self->cmap_ranges[i].glyph_offset + glyph_id;
                }

                case 2: // Range to range - calculate based on offset within range
                    uint16_t idx = codepoint - self->cmap_ranges[i].range_start;
                    uint16_t glyph_id = self->cmap_ranges[i].glyph_offset + idx;
                    return glyph_id;

                case 3: { // Direct mapping - need to look up in the table
                    if (!self->file_is_open) {
                        return -1;
                    }

                    FRESULT res;
                    res = f_lseek(&self->file, self->cmap_ranges[i].data_offset);
                    if (res != FR_OK) {
                        return -1;
                    }
                    uint16_t codepoint_delta = codepoint - self->cmap_ranges[i].range_start;

                    for (size_t j = 0; j < self->cmap_ranges[i].entries_count; j++) {
                        // Read code point at the index
                        uint16_t candidate_codepoint_delta;
                        UINT bytes_read;
                        res = f_read(&self->file, &candidate_codepoint_delta, 2, &bytes_read);
                        if (res != FR_OK || bytes_read < 2) {
                            return -1;
                        }

                        if (candidate_codepoint_delta == codepoint_delta) {
                            return self->cmap_ranges[i].glyph_offset + j;
                        }
                    }
                    return -1;
                }

                default:
                    return -1;
            }
        }
    }

    return -1; // Not found
}

// CIRCUITPY-CHANGE: locating a codepoint's glyph data was inline in cache_glyph. It is
// shared now, because the width of a glyph has to be answerable without caching it.
// Leaves the file positioned for read_glyph_dimensions. The file must already be open.
static bool seek_to_glyph_data(lvfontio_ondiskfont_t *self, uint32_t codepoint) {
    int32_t char_id = get_char_id(self, codepoint);
    if (char_id < 0 || (uint32_t)char_id >= self->max_cid) {
        return false; // Invalid character
    }

    // Get glyph offset from location table
    uint32_t loca_offset = self->loca_table_offset + char_id *
        (self->header.index_to_loc_format == 1 ? 4 : 2);

    FRESULT res = f_lseek(&self->file, loca_offset);
    if (res != FR_OK) {
        return false;
    }

    uint32_t glyph_offset = 0;
    UINT bytes_read;
    if (self->header.index_to_loc_format == 1) {
        // 4-byte offset
        uint8_t offset_buf[4];
        res = f_read(&self->file, offset_buf, 4, &bytes_read);
        if (res != FR_OK || bytes_read < 4) {
            return false;
        }
        glyph_offset = offset_buf[0] | (offset_buf[1] << 8) |
            (offset_buf[2] << 16) | (offset_buf[3] << 24);
    } else {
        // 2-byte offset
        uint8_t offset_buf[2];
        res = f_read(&self->file, offset_buf, 2, &bytes_read);
        if (res != FR_OK || bytes_read < 2) {
            return false;
        }
        glyph_offset = offset_buf[0] | (offset_buf[1] << 8);
    }

    // Seek to glyph data
    return f_lseek(&self->file, self->glyf_table_offset + glyph_offset) == FR_OK;
}

// Load glyph bitmap data into a slot
// This function assumes the file is already open and positioned after reading the glyph dimensions
static bool load_glyph_bitmap(FIL *file, lvfontio_ondiskfont_t *self, uint32_t codepoint, uint16_t slot,
    uint32_t glyph_advance, int32_t bbox_x, int32_t bbox_y, uint32_t bbox_w, uint32_t bbox_h,
    uint8_t *byte_val, uint8_t *remaining_bits) {
    // Store codepoint at slot
    self->codepoints[slot] = codepoint;
    self->reference_counts[slot] = 1;

    // Read bitmap data pixel by pixel
    uint16_t x_offset = slot * self->header.default_advance_width;
    uint16_t y_offset = self->header.ascent - bbox_y - bbox_h;
    for (uint16_t y = 0; y < bbox_h; y++) {
        for (uint16_t x = 0; x < bbox_w; x++) {
            uint32_t pixel_value;
            FRESULT res = read_bits(file, self->header.bits_per_pixel, byte_val, remaining_bits, &pixel_value);
            if (res != FR_OK) {
                // CIRCUITPY-CHANGE: the claim staked out above was left behind on
                // this path, so cache_glyph returned -1 with a reference already
                // taken. Callers reasonably read -1 as "nothing was cached" and
                // reacquire the slot they had released, which then sat at two
                // references for a single cell and could never be reused. Undo the
                // claim so -1 really does mean no reference was taken.
                self->codepoints[slot] = LVFONTIO_INVALID_CODEPOINT;
                self->reference_counts[slot] = 0;
                return false;
            }

            // Adjust for bbox position within the glyph bounding box
            int16_t bitmap_x = x_offset + x + bbox_x;
            int16_t bitmap_y = y_offset + y;

            // Make sure we're in bounds
            if (bitmap_x >= 0 &&
                bitmap_x < self->header.default_advance_width * self->max_glyphs &&
                bitmap_y >= 0 &&
                bitmap_y < self->header.font_size) {
                common_hal_displayio_bitmap_set_pixel(
                    self->bitmap,
                    bitmap_x,
                    bitmap_y,
                    pixel_value
                    );
            }
        }
    }

    return true;
}

// Constructor
void common_hal_lvfontio_ondiskfont_construct(lvfontio_ondiskfont_t *self,
    const char *file_path,
    uint16_t max_glyphs,
    bool use_gc_allocator) {

    // Store the allocation mode
    self->use_gc_allocator = use_gc_allocator;
    // Store parameters
    self->file_path = file_path; // Store the provided path string directly
    self->max_glyphs = max_glyphs;
    self->cmap_ranges = NULL;
    // CIRCUITPY-CHANGE: allocate_memory() runs deinit before returning NULL, and
    // deinit frees these three. The supervisor builds its font in a stack local
    // (supervisor/shared/display.c), which port_malloc does not zero, so a failed
    // allocation deinited through indeterminate pointers long before any caller
    // could check the NULL. Only cmap_ranges was cleared here.
    self->bitmap = NULL;
    self->codepoints = NULL;
    self->reference_counts = NULL;
    self->file_is_open = false;

    // Determine which filesystem to use based on the path
    const char *path_under_mount;
    fs_user_mount_t *vfs = filesystem_for_path(file_path, &path_under_mount);

    if (vfs == NULL) {
        if (self->use_gc_allocator) {
            mp_raise_ValueError(MP_ERROR_TEXT("File not found"));
        }
        return;
    }

    // Open the file and keep it open for the lifetime of the object
    FRESULT res = f_open(&vfs->fatfs, &self->file, path_under_mount, FA_READ);

    if (res != FR_OK) {
        if (self->use_gc_allocator) {
            mp_raise_ValueError(MP_ERROR_TEXT("File not found"));
        }
        return;
    }

    self->file_is_open = true;

    // Load font headers
    size_t max_slots;
    if (!load_font_header(self, &self->file, &max_slots)) {
        f_close(&self->file);
        self->file_is_open = false;
        if (self->use_gc_allocator) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_file);
        }
        return;
    }
    // Cap the number of slots to the number of slots needed by the font. That way
    // small font files don't need a bunch of extra cache space.
    max_glyphs = MIN(max_glyphs, max_slots);
    // CIRCUITPY-CHANGE: self->max_glyphs was set from the uncapped argument above
    // while everything below is sized with the capped one. The supervisor asks for
    // width_in_tiles * height_in_tiles slots, around 240 on this board, so any font
    // whose header census yields fewer made every later scan of codepoints[] and
    // reference_counts[] run off the end of its allocation, and load_glyph_bitmap
    // wrote past them.
    self->max_glyphs = max_glyphs;

    // Allocate codepoints array. allocate_memory will raise an exception if
    // allocation fails and the VM is active.
    self->codepoints = allocate_memory(self, sizeof(uint32_t) * max_glyphs);
    if (self->codepoints == NULL) {
        return;
    }

    // Initialize codepoints to invalid
    for (uint16_t i = 0; i < max_glyphs; i++) {
        self->codepoints[i] = LVFONTIO_INVALID_CODEPOINT;
    }

    // Allocate reference counts
    self->reference_counts = allocate_memory(self, sizeof(uint16_t) * max_glyphs);
    if (self->reference_counts == NULL) {
        return;
    }

    // Initialize reference counts to 0
    memset(self->reference_counts, 0, sizeof(uint16_t) * max_glyphs);

    self->half_width_px = self->header.default_advance_width;

    // Create bitmap for glyph cache
    displayio_bitmap_t *bitmap = allocate_memory(self, sizeof(displayio_bitmap_t));
    // CIRCUITPY-CHANGE: the type was stored before the null check, so a failed
    // allocation wrote through a null pointer instead of returning. allocate_memory
    // only returns NULL when the VM is not active to raise, i.e. on the supervisor
    // path, which is exactly where a crash is hardest to read.
    if (bitmap == NULL) {
        return;
    }
    bitmap->base.type = &displayio_bitmap_type;

    // CIRCUITPY-CHANGE: the header field is the real bits per pixel, 1 to 4 -- that
    // is what read_bits() is handed when the glyph is decoded. Using
    // "1 << bits_per_pixel" as the bitmap's storage depth therefore asked for 2, 4,
    // 8 or 16 bits where 1, 2, 4 and 4 are needed, so the glyph cache took two to
    // four times the RAM it should. displayio stores at 1, 2 or 4 bits, so round the
    // source depth up to the nearest of those.
    uint32_t bits_per_pixel = self->header.bits_per_pixel > 2 ? 4 : self->header.bits_per_pixel;
    uint32_t width = self->header.default_advance_width * max_glyphs;
    uint32_t row_width = width * bits_per_pixel;
    uint16_t stride = (row_width + 31) / 32; // Align to uint32_t (32 bits)

    // Allocate buffer for bitmap data
    uint32_t buffer_size = stride * self->header.font_size * sizeof(uint32_t);
    uint32_t *bitmap_buffer = allocate_memory(self, buffer_size);
    if (bitmap_buffer == NULL) {
        return;
    }

    // Zero out bitmap buffer
    memset(bitmap_buffer, 0, buffer_size);

    // Construct bitmap with allocated buffer
    common_hal_displayio_bitmap_construct_from_buffer(bitmap,
        self->header.default_advance_width * max_glyphs,
        self->header.font_size,
        bits_per_pixel,
            bitmap_buffer,
            false);
    self->bitmap = bitmap;
}

void common_hal_lvfontio_ondiskfont_deinit(lvfontio_ondiskfont_t *self) {
    if (!self->file_is_open) {
        return;
    }

    if (self->bitmap != NULL) {
        common_hal_displayio_bitmap_deinit(self->bitmap);
        self->bitmap = NULL;
    }

    if (self->codepoints != NULL) {
        free_memory(self, self->codepoints);
        self->codepoints = NULL;
    }

    if (self->reference_counts != NULL) {
        free_memory(self, self->reference_counts);
        self->reference_counts = NULL;
    }



    if (self->cmap_ranges != NULL) {
        free_memory(self, self->cmap_ranges);
        self->cmap_ranges = NULL;
    }

    f_close(&self->file);
    self->file_is_open = false;
}

bool common_hal_lvfontio_ondiskfont_deinited(lvfontio_ondiskfont_t *self) {
    return !self->file_is_open;
}

mp_obj_t common_hal_lvfontio_ondiskfont_get_bitmap(const lvfontio_ondiskfont_t *self) {
    return MP_OBJ_FROM_PTR(self->bitmap);
}

mp_obj_t common_hal_lvfontio_ondiskfont_get_bounding_box(const lvfontio_ondiskfont_t *self) {
    mp_obj_t bbox[2];
    bbox[0] = MP_OBJ_NEW_SMALL_INT(self->header.default_advance_width);
    bbox[1] = MP_OBJ_NEW_SMALL_INT(self->header.font_size);
    return mp_obj_new_tuple(2, bbox);
}

void common_hal_lvfontio_ondiskfont_get_dimensions(const lvfontio_ondiskfont_t *self,
    uint16_t *width, uint16_t *height) {
    if (width != NULL) {
        *width = self->header.default_advance_width;
    }
    if (height != NULL) {
        *height = self->header.font_size;
    }
}

// CIRCUITPY-CHANGE: a full-width glyph needs two adjacent free slots, so the terminal has
// to release both of the cells it is about to overwrite before cache_glyph runs, and it
// can only know that there are two by asking first. This makes the same advance width
// test cache_glyph makes, without claiming a slot or reading a bitmap.
bool common_hal_lvfontio_ondiskfont_is_full_width(lvfontio_ondiskfont_t *self, uint32_t codepoint) {
    int16_t existing_slot = find_codepoint_slot(self, codepoint);
    if (existing_slot >= 0) {
        // Answer from the cache the same way cache_glyph does, so the two agree.
        uint16_t next_slot = (existing_slot + 1) % self->max_glyphs;
        return self->codepoints[next_slot] == codepoint;
    }

    if (self->header.glyph_advance_bits == 0) {
        // The advance is not stored per glyph, so read_glyph_dimensions would hand back
        // the default for every one of them. Answer without touching the file.
        return self->header.default_advance_width > self->half_width_px;
    }

    if (!self->file_is_open || !seek_to_glyph_data(self, codepoint)) {
        return false;
    }

    uint32_t glyph_advance;
    int32_t bbox_x, bbox_y;
    uint32_t bbox_w, bbox_h;
    uint8_t byte_val = 0;
    uint8_t remaining_bits = 0;
    // A glyph whose header cannot be read cannot be cached either, so the caller ends up
    // on its missing glyph path having released only the one cell, as it did before.
    if (read_glyph_dimensions(&self->file, self, &glyph_advance, &bbox_x, &bbox_y,
        &bbox_w, &bbox_h, &byte_val, &remaining_bits) != FR_OK) {
        return false;
    }

    return glyph_advance > self->half_width_px;
}

int16_t common_hal_lvfontio_ondiskfont_cache_glyph(lvfontio_ondiskfont_t *self, uint32_t codepoint, bool *is_full_width) {
    // Check if already cached
    int16_t existing_slot = find_codepoint_slot(self, codepoint);
    if (existing_slot >= 0) {
        // Glyph is already cached, increment reference count(s).
        self->reference_counts[existing_slot]++;

        // Check if this is a full-width character by looking for a second slot
        // with the same codepoint right after this one, wrapping at the end.
        uint16_t next_slot = (existing_slot + 1) % self->max_glyphs;
        bool cached_is_full_width = self->codepoints[next_slot] == codepoint;

        if (cached_is_full_width) {
            self->reference_counts[next_slot]++;
        }

        if (is_full_width != NULL) {
            *is_full_width = cached_is_full_width;
        }

        return existing_slot;
    }

    // First check if the glyph is full-width before allocating slots
    // This way we know if we need one or two slots before committing
    bool is_full_width_glyph = false;

    // Check if file is already open
    if (!self->file_is_open) {

        return -1;
    }

    if (!seek_to_glyph_data(self, codepoint)) {
        return -1;
    }

    // Read glyph header fields to determine width
    uint32_t glyph_advance;
    int32_t bbox_x, bbox_y;
    uint32_t bbox_w, bbox_h;

    // Initialize bit reading state
    uint8_t byte_val = 0;
    uint8_t remaining_bits = 0;

    // Use the helper function to read glyph dimensions
    FRESULT res = read_glyph_dimensions(&self->file, self, &glyph_advance, &bbox_x, &bbox_y, &bbox_w, &bbox_h, &byte_val, &remaining_bits);
    if (res != FR_OK) {
        return -1;
    }

    // Check if the glyph is full-width based on its advance width
    // Full-width characters typically have an advance width close to or greater than the font height
    is_full_width_glyph = glyph_advance > self->half_width_px;

    // Now we know if we need one or two slots
    uint16_t slots_needed = is_full_width_glyph ? 2 : 1;

    // Find an appropriate slot (or consecutive slots for full-width)
    uint16_t slot = UINT16_MAX;

    // CIRCUITPY-CHANGE: full-width glyphs had a slot search of their own that only
    // accepted two adjacent slots still marked INVALID, started at 0 and never wrapped.
    // codepoints[] is reset to INVALID only at construction and half-width slots are
    // handed out from codepoint % max_glyphs, so the adjacent INVALID pairs were gone
    // once roughly half the slots had been touched, and from then on no full-width glyph
    // could be cached even with the whole cache unreferenced and evictable.
    slot = find_free_slot(self, codepoint, slots_needed);

    // Check if we found appropriate slot(s)
    if (slot == UINT16_MAX) {
        return -1; // No slots available
    }

    // CIRCUITPY-CHANGE: taking over one half of an unreferenced full-width pair left the
    // other half still carrying that codepoint with no bitmap behind it, so the next
    // lookup for that character matched the leftover and drew half of the old glyph in a
    // cell of its own. codepoints[] was only ever reset at construction.
    for (uint16_t i = 0; i < slots_needed; i++) {
        invalidate_full_width_partner(self, slot + i);
    }

    // Load glyph into the slot
    if (!load_glyph_bitmap(&self->file, self, codepoint, slot, glyph_advance,
        bbox_x, bbox_y, bbox_w, bbox_h, &byte_val, &remaining_bits)) {
        return -1; // Failed to load glyph
    }

    // For full-width characters, mark both slots with the same codepoint
    if (is_full_width_glyph && slot + 1 < self->max_glyphs) {
        self->codepoints[slot + 1] = codepoint;
        self->reference_counts[slot + 1] = 1;
    }

    if (is_full_width != NULL) {
        *is_full_width = is_full_width_glyph;
    }

    return slot;
}

void common_hal_lvfontio_ondiskfont_release_glyph(lvfontio_ondiskfont_t *self, uint32_t slot) {
    if (slot >= self->max_glyphs) {
        return;
    }

    if (self->reference_counts[slot] > 0) {
        self->reference_counts[slot]--;
    }
}

// CIRCUITPY-CHANGE: terminalio has to release a cell's slot before it knows whether the
// replacement glyph can be cached at all, and there was no way to take that reference
// back when it could not, which left a slot that is still on screen looking unused.
void common_hal_lvfontio_ondiskfont_retain_glyph(lvfontio_ondiskfont_t *self, uint32_t slot) {
    if (slot >= self->max_glyphs) {
        return;
    }

    self->reference_counts[slot]++;
}

static int16_t find_codepoint_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint) {
    size_t offset = codepoint % self->max_glyphs;
    for (uint16_t i = 0; i < self->max_glyphs; i++) {
        int16_t slot = (i + offset) % self->max_glyphs;
        if (self->codepoints[slot] == codepoint) {
            // If this is the second slot of a full-width glyph pair, return the
            // first slot so callers always get a canonical index.
            if (slot > 0 && self->codepoints[slot - 1] == codepoint) {
                return slot - 1;
            }
            return slot;
        }
    }
    return -1;
}

static bool slot_has_active_full_width_partner(lvfontio_ondiskfont_t *self, uint16_t slot) {
    uint32_t codepoint = self->codepoints[slot];
    if (codepoint == LVFONTIO_INVALID_CODEPOINT) {
        return false;
    }

    // Don't evict one half of a full-width pair while the other half is still in use.
    uint16_t prev_slot = (slot + self->max_glyphs - 1) % self->max_glyphs;
    uint16_t next_slot = (slot + 1) % self->max_glyphs;

    if (self->codepoints[prev_slot] == codepoint && self->reference_counts[prev_slot] > 0) {
        return true;
    }
    if (self->codepoints[next_slot] == codepoint && self->reference_counts[next_slot] > 0) {
        return true;
    }

    return false;
}

// Claiming a slot that was one half of a full-width pair leaves the other half pointing
// at a glyph that is no longer whole.
static void invalidate_full_width_partner(lvfontio_ondiskfont_t *self, uint16_t slot) {
    uint32_t codepoint = self->codepoints[slot];
    if (codepoint == LVFONTIO_INVALID_CODEPOINT) {
        return;
    }

    uint16_t prev_slot = (slot + self->max_glyphs - 1) % self->max_glyphs;
    uint16_t next_slot = (slot + 1) % self->max_glyphs;

    if (self->codepoints[prev_slot] == codepoint) {
        self->codepoints[prev_slot] = LVFONTIO_INVALID_CODEPOINT;
    }
    if (self->codepoints[next_slot] == codepoint) {
        self->codepoints[next_slot] = LVFONTIO_INVALID_CODEPOINT;
    }
}

static uint16_t find_free_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint, uint16_t slots_needed) {
    size_t offset = codepoint % self->max_glyphs;

    // First look for completely unused slots, starting at the offset
    for (uint16_t i = 0; i < self->max_glyphs; i++) {
        uint16_t slot = (i + offset) % self->max_glyphs;
        // The slots of one glyph have to be consecutive: the bitmap column and the tile
        // index of the second half are both the first plus one, so a run must not wrap
        // around the end of the cache even though the search does.
        if (slot + slots_needed > self->max_glyphs) {
            continue;
        }
        bool usable = true;
        for (uint16_t j = 0; j < slots_needed; j++) {
            if (self->codepoints[slot + j] != LVFONTIO_INVALID_CODEPOINT ||
                self->reference_counts[slot + j] != 0) {
                usable = false;
                break;
            }
        }
        if (usable) {
            return slot;
        }
    }

    // If none found, look for slots with zero reference count, starting at the offset.
    // Avoid reusing one half of an active full-width glyph pair.
    for (uint16_t i = 0; i < self->max_glyphs; i++) {
        uint16_t slot = (i + offset) % self->max_glyphs;
        if (slot + slots_needed > self->max_glyphs) {
            continue;
        }
        bool usable = true;
        for (uint16_t j = 0; j < slots_needed; j++) {
            if (self->reference_counts[slot + j] != 0 ||
                slot_has_active_full_width_partner(self, slot + j)) {
                usable = false;
                break;
            }
        }
        if (usable) {
            return slot;
        }
    }

    // No slots available
    return UINT16_MAX;
}

static FRESULT read_glyph_dimensions(FIL *file, lvfontio_ondiskfont_t *self,
    uint32_t *advance_width, int32_t *bbox_x, int32_t *bbox_y,
    uint32_t *bbox_w, uint32_t *bbox_h,
    uint8_t *byte_val, uint8_t *remaining_bits) {
    FRESULT res;
    uint32_t temp_value;

    // Read glyph_advance
    res = read_bits(file, self->header.glyph_advance_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    // CIRCUITPY-CHANGE: glyph_advance_bits of 0 does not mean every glyph is zero
    // width, it means the advance is not stored per glyph because they all share
    // default_advance_width -- which is what lv_font_conv emits for a monospace
    // font, the obvious choice for a terminal. read_bits answers 0 without reading
    // anything, so the slot census discarded every glyph as zero-advance and
    // reported one usable slot: the whole terminal shared a single glyph.
    *advance_width = self->header.glyph_advance_bits == 0
        ? self->header.default_advance_width
        : temp_value;

    // Read bbox_x (signed)
    res = read_bits(file, self->header.glyph_bbox_xy_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    // Convert to signed value if needed
    if (temp_value & (1 << (self->header.glyph_bbox_xy_bits - 1))) {
        *bbox_x = temp_value - (1 << self->header.glyph_bbox_xy_bits);
    } else {
        *bbox_x = temp_value;
    }

    // Read bbox_y (signed)
    res = read_bits(file, self->header.glyph_bbox_xy_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    // Convert to signed value if needed
    if (temp_value & (1 << (self->header.glyph_bbox_xy_bits - 1))) {
        *bbox_y = temp_value - (1 << self->header.glyph_bbox_xy_bits);
    } else {
        *bbox_y = temp_value;
    }

    // Read bbox_w
    res = read_bits(file, self->header.glyph_bbox_wh_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    *bbox_w = temp_value;

    // Read bbox_h
    res = read_bits(file, self->header.glyph_bbox_wh_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    *bbox_h = temp_value;

    return FR_OK;
}

static FRESULT read_bits(FIL *file, size_t num_bits, uint8_t *byte_val, uint8_t *remaining_bits, uint32_t *result) {
    FRESULT res = FR_OK;
    UINT bytes_read;

    uint32_t value = 0;
    // Bits will be lost when num_bits > 32. However, this is good for skipping bits.
    size_t bits_needed = num_bits;

    while (bits_needed > 0) {
        // If no bits remaining, read a new byte
        if (*remaining_bits == 0) {
            res = f_read(file, byte_val, 1, &bytes_read);
            if (res != FR_OK || bytes_read < 1) {
                return FR_DISK_ERR;
            }
            *remaining_bits = 8;
        }

        // Calculate how many bits to take from current byte
        uint8_t bits_to_take = (*remaining_bits < bits_needed) ? *remaining_bits : bits_needed;
        value = (value << bits_to_take) | (*byte_val >> (8 - bits_to_take));

        // Update state
        *remaining_bits -= bits_to_take;
        bits_needed -= bits_to_take;

        // Shift byte for next read
        *byte_val <<= bits_to_take;
        *byte_val &= 0xFF;
    }

    if (result != NULL) {
        *result = value;
    }
    return FR_OK;
}
