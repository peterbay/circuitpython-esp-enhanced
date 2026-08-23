// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-module/terminalio/Terminal.h"

#include "shared-module/fontio/BuiltinFont.h"
#include "shared-bindings/displayio/TileGrid.h"
#include "shared-bindings/displayio/Palette.h"
#include "shared-bindings/terminalio/Terminal.h"
#include "shared-bindings/fontio/BuiltinFont.h"
#if CIRCUITPY_LVFONTIO
#include "shared-bindings/lvfontio/OnDiskFont.h"
#endif

#if CIRCUITPY_STATUS_BAR
#include "shared-bindings/supervisor/__init__.h"
#include "shared-bindings/supervisor/StatusBar.h"
#endif

#include "supervisor/shared/serial.h"

uint16_t terminalio_terminal_get_glyph_index(mp_obj_t font, mp_uint_t codepoint, bool *is_full_width) {
    if (is_full_width != NULL) {
        *is_full_width = false;  // Default to not full width
    }

    #if CIRCUITPY_LVFONTIO
    if (mp_obj_is_type(font, &lvfontio_ondiskfont_type)) {
        // For LV fonts, we need to cache the glyph first
        lvfontio_ondiskfont_t *lv_font = MP_OBJ_TO_PTR(font);
        bool full_width = false;
        int16_t slot = common_hal_lvfontio_ondiskfont_cache_glyph(lv_font, codepoint, &full_width);

        if (is_full_width != NULL) {
            *is_full_width = full_width;
        }

        if (slot == -1) {
            // Not found or couldn't cache
            return 0xffff;
        }
        return (uint16_t)slot;
    }
    #endif

    #if CIRCUITPY_FONTIO
    if (mp_obj_is_type(font, &fontio_builtinfont_type)) {
        // Use the standard fontio function
        fontio_builtinfont_t *fontio_font = MP_OBJ_TO_PTR(font);
        uint8_t index = fontio_builtinfont_get_glyph_index(fontio_font, codepoint);
        if (index == 0xff) {
            return 0xffff;
        }
        return index;
    }
    #endif

    // Unsupported font type
    return 0xffff;
}

static void wrap_cursor(uint16_t width, uint16_t height, uint16_t *cursor_x, uint16_t *cursor_y) {
    if (*cursor_x >= width) {
        *cursor_y = *cursor_y + 1;
        *cursor_x %= width;
    }
    if (*cursor_y >= height) {
        *cursor_y %= height;
    }
}

static void release_current_glyph(displayio_tilegrid_t *tilegrid, mp_obj_t font, uint16_t x, uint16_t y) {
    #if CIRCUITPY_LVFONTIO
    if (!mp_obj_is_type(font, &lvfontio_ondiskfont_type)) {
        return;
    }
    uint16_t current_tile = common_hal_displayio_tilegrid_get_tile(tilegrid, x, y);
    // CIRCUITPY-CHANGE: an empty "if (current_tile == 0) {}" stood here. It is dropped
    // rather than given a body: lvfontio hands out slot 0 like any other, so a cell
    // showing it owns a reference that has to come back. Skipping slot 0 would pin it for
    // the lifetime of the font and let its uint16_t count wrap once the character living
    // there had been written 65536 times, after which it would be evicted on screen.
    common_hal_lvfontio_ondiskfont_release_glyph(MP_OBJ_TO_PTR(font), current_tile);
    #endif
}

// CIRCUITPY-CHANGE: the release above deliberately happens before the replacement glyph
// is cached, so that on a full terminal the outgoing cell's slot can be reused by the
// incoming one. But every path that then left the cell unchanged kept the lost reference,
// so a slot still on screen looked unused and was handed out under the cell displaying
// it. Those paths give the reference back through here.
static void reacquire_current_glyph(displayio_tilegrid_t *tilegrid, mp_obj_t font, uint16_t x, uint16_t y) {
    #if CIRCUITPY_LVFONTIO
    if (!mp_obj_is_type(font, &lvfontio_ondiskfont_type)) {
        return;
    }
    uint16_t current_tile = common_hal_displayio_tilegrid_get_tile(tilegrid, x, y);
    common_hal_lvfontio_ondiskfont_retain_glyph(MP_OBJ_TO_PTR(font), current_tile);
    #endif
}

// CIRCUITPY-CHANGE: how many cells a character covers has to be known before any cell is
// released, so that a full-width glyph has both of the slots it needs free by the time it
// is cached. Fonts without full-width glyphs answer false without any work.
static bool character_is_full_width(mp_obj_t font, mp_uint_t character) {
    #if CIRCUITPY_LVFONTIO
    if (mp_obj_is_type(font, &lvfontio_ondiskfont_type)) {
        return common_hal_lvfontio_ondiskfont_is_full_width(MP_OBJ_TO_PTR(font), character);
    }
    #endif
    return false;
}

static void terminalio_terminal_set_tile(terminalio_terminal_obj_t *self, bool status_bar, mp_uint_t character, bool release_glyphs) {
    displayio_tilegrid_t *tilegrid = self->scroll_area;
    uint16_t *x = &self->cursor_x;
    uint16_t *y = &self->cursor_y;
    uint16_t w = self->scroll_area->width_in_tiles;
    uint16_t h = self->scroll_area->height_in_tiles;
    if (status_bar) {
        tilegrid = self->status_bar;
        x = &self->status_x;
        y = &self->status_y;
        w = self->status_bar->width_in_tiles;
        h = self->status_bar->height_in_tiles;
    }
    // CIRCUITPY-CHANGE: only the cell under the cursor was released before the glyph was
    // cached, and the second cell of a full-width glyph not until afterwards. A full-width
    // glyph needs two adjacent free slots, which a single release can never produce, so on
    // a saturated cache it was refused and the cell kept its old character. Ask how wide
    // the character is first and release every cell it will cover, which is what frees the
    // slot pair the outgoing full-width character was holding.
    bool wide = character_is_full_width(self->font, character);

    // If there is only half width left, then fill it with a space and wrap to the next line.
    if (wide && *x == w - 1) {
        if (release_glyphs) {
            release_current_glyph(tilegrid, self->font, *x, *y);
        }
        uint16_t space = terminalio_terminal_get_glyph_index(self->font, ' ', NULL);
        // CIRCUITPY-CHANGE: the space was stored without checking for the missing-glyph
        // marker. 0xffff is past tiles_in_bitmap, so set_tile raised ValueError with no
        // nlr handler above it on the supervisor's terminal, and it is the other way the
        // cell released just above was left unreplaced with its reference gone.
        if (space != 0xffff) {
            common_hal_displayio_tilegrid_set_tile(tilegrid, *x, *y, space);
        } else if (release_glyphs) {
            reacquire_current_glyph(tilegrid, self->font, *x, *y);
        }
        *x = *x + 1;
        wrap_cursor(w, h, x, y);
    }

    // The cell after the cursor, which a full-width character covers as well.
    uint16_t second_x = *x + 1;
    uint16_t second_y = *y;
    wrap_cursor(w, h, &second_x, &second_y);
    bool second_released = wide && release_glyphs;
    if (release_glyphs) {
        release_current_glyph(tilegrid, self->font, *x, *y);
        if (second_released) {
            release_current_glyph(tilegrid, self->font, second_x, second_y);
        }
    }

    bool is_full_width;
    uint16_t new_tile = terminalio_terminal_get_glyph_index(self->font, character, &is_full_width);
    if (new_tile == 0xffff) {
        // Missing glyph. Nothing was cached, so both cells keep what they were showing and
        // both released references go back to the slots that are still on screen.
        if (release_glyphs) {
            reacquire_current_glyph(tilegrid, self->font, *x, *y);
            if (second_released) {
                reacquire_current_glyph(tilegrid, self->font, second_x, second_y);
            }
        }
        return;
    }
    common_hal_displayio_tilegrid_set_tile(tilegrid, *x, *y, new_tile);
    *x = *x + 1;
    wrap_cursor(w, h, x, y);
    if (is_full_width) {
        // The width above is a prediction, so the second cell may still hold its reference.
        if (release_glyphs && !second_released) {
            release_current_glyph(tilegrid, self->font, *x, *y);
        }
        common_hal_displayio_tilegrid_set_tile(tilegrid, *x, *y, new_tile + 1);
        *x = *x + 1;
        wrap_cursor(w, h, x, y);
    } else if (second_released) {
        // Released for a character that did not turn out to be full width after all. The
        // cell is left as it was, so it keeps its reference.
        reacquire_current_glyph(tilegrid, self->font, second_x, second_y);
    }
}

// Helper function to set all tiles in a tilegrid with optional glyph release
static void terminalio_terminal_set_all_tiles(terminalio_terminal_obj_t *self, bool status_bar, mp_uint_t character, bool release_glyphs) {
    uint16_t *x = &self->cursor_x;
    uint16_t *y = &self->cursor_y;
    if (status_bar) {
        x = &self->status_x;
        y = &self->status_y;
    }
    *x = 0;
    *y = 0;
    terminalio_terminal_set_tile(self, status_bar, character, release_glyphs);
    while (*x != 0 || *y != 0) {
        terminalio_terminal_set_tile(self, status_bar, character, release_glyphs);
    }
}

void terminalio_terminal_clear_status_bar(terminalio_terminal_obj_t *self) {
    if (self->status_bar) {
        terminalio_terminal_set_all_tiles(self, true, ' ', true);
    }
}


void common_hal_terminalio_terminal_construct(terminalio_terminal_obj_t *self,
    displayio_tilegrid_t *scroll_area, mp_obj_t font,
    displayio_tilegrid_t *status_bar) {
    self->cursor_x = 0;
    self->cursor_y = 0;
    self->font = font;
    self->scroll_area = scroll_area;
    self->status_bar = status_bar;
    self->status_x = 0;
    self->status_y = 0;
    self->first_row = 0;
    self->vt_scroll_top = 0;
    self->vt_scroll_end = self->scroll_area->height_in_tiles - 1;
    terminalio_terminal_set_all_tiles(self, false, ' ', false);
    if (self->status_bar) {
        terminalio_terminal_set_all_tiles(self, true, ' ', false);
    }

    common_hal_displayio_tilegrid_set_top_left(self->scroll_area, 0, 1);
}

size_t common_hal_terminalio_terminal_write(terminalio_terminal_obj_t *self, const byte *data, size_t len, int *errcode) {
    #define SCRNMOD(x) (((x) + (self->scroll_area->top_left_y)) % (self->scroll_area->height_in_tiles))

    // Make sure the terminal is initialized before we do anything with it.
    if (self->scroll_area == NULL) {
        return len;
    }

    #if CIRCUITPY_TERMINALIO_VT100
    uint32_t _select_color(uint16_t ascii_color) {
        uint32_t color_value = 0;
        if ((ascii_color & 1) > 0) {
            color_value += 0xff0000;
        }
        if ((ascii_color & 2) > 0) {
            color_value += 0x00ff00;
        }
        if ((ascii_color & 4) > 0) {
            color_value += 0x0000ff;
        }

        return color_value;
    }

    displayio_palette_t *terminal_palette = self->scroll_area->pixel_shader;
    #endif

    const byte *i = data;
    uint16_t start_y = self->cursor_y;

    // CIRCUITPY-CHANGE: the escape parser below looks ahead as far as i[11] with no
    // regard for the end of the buffer, and then advances i by the length the
    // sequence would have had, which could take i past the end. mp_stream_rw
    // subtracts the return value from an unsigned remaining count, so a return
    // larger than len wrapped it and the write loop never terminated: a hang, with
    // heap contents rendered onto the display on the way. A read past the end now
    // yields 0, which matches no branch below and ends the digit loops, and every
    // advance saturates at the end so i - data can never exceed len. Malformed and
    // truncated sequences were undefined before, so only they change behaviour.
    // Not covered here: utf8_get_char/utf8_next_char at the top of the loop take no
    // length and can still walk continuation bytes past the end. That is a
    // pre-existing hole needing a cursor both the decoder and the parser share; the
    // saturation below at least keeps it from reaching the caller.
    #define TERM_PEEK(offset) ((i) + (offset) < data + len ? (i)[offset] : 0)
    #define TERM_ADVANCE(n) do { \
        size_t _avail = (size_t)(data + len - i); \
        size_t _n = (n); \
        i += (_n < _avail ? _n : _avail); \
} while (0)

    while (i < data + len) {
        unichar c = utf8_get_char(i);
        i = utf8_next_char(i);
        if (i > data + len) {
            i = data + len;
        }
        if (self->in_osc_command) {
            if (c == 0x1b && TERM_PEEK(0) == '\\') {
                self->in_osc_command = false;
                self->status_x = 0;
                self->status_y = 0;
                i += 1;
            } else if (
                self->osc_command == 0 &&
                self->status_bar != NULL &&
                self->status_y < self->status_bar->height_in_tiles) {
                // Clear the tile grid before we start putting new info.
                if (self->status_x == 0 && self->status_y == 0) {
                    terminalio_terminal_set_all_tiles(self, true, ' ', true);
                }
                terminalio_terminal_set_tile(self, true, c, true);
            }
            continue;
        }
        if (c < 0x20) {
            if (c == '\r') {
                self->cursor_x = 0;
            } else if (c == '\t') {
                for (uint8_t space_i = 0; space_i < 4; space_i++) {
                    terminalio_terminal_set_tile(self, false, ' ', true);
                }
            } else if (c == '\n') {
                self->cursor_y++;
                // Commands below are used by MicroPython in the REPL
            } else if (c == '\b') {
                if (self->cursor_x > 0) {
                    self->cursor_x--;
                }
            } else if (c == 0x1b) {
                // Handle commands of the form [ESC].<digits><command-char> where . is not yet known.
                int16_t vt_args[3] = {0, 0, 0};
                uint8_t j = 1;
                #if CIRCUITPY_TERMINALIO_VT100
                uint8_t n_args = 1;
                #endif
                for (; j < 6; j++) {
                    byte d = TERM_PEEK(j);
                    if ('0' <= d && d <= '9') {
                        vt_args[0] = vt_args[0] * 10 + (d - '0');
                    } else {
                        c = d;
                        break;
                    }
                }
                if (TERM_PEEK(0) == '[') {
                    for (uint8_t i_args = 1; i_args < 3 && c == ';'; i_args++) {
                        vt_args[i_args] = 0;
                        for (++j; j < 12; j++) {
                            byte d = TERM_PEEK(j);
                            if ('0' <= d && d <= '9') {
                                vt_args[i_args] = vt_args[i_args] * 10 + (d - '0');
                                #if CIRCUITPY_TERMINALIO_VT100
                                n_args = i_args + 1;
                                #endif
                            } else {
                                c = d;
                                break;
                            }
                        }
                    }
                    if (c == '?') {
                        #if CIRCUITPY_TERMINALIO_VT100
                        if (TERM_PEEK(2) == '2' && TERM_PEEK(3) == '5') {
                            // cursor visibility commands
                            if (TERM_PEEK(4) == 'h') {
                                // make cursor visible
                                // not implemented yet
                            } else if (TERM_PEEK(4) == 'l') {
                                // make cursor invisible
                                // not implemented yet
                            }
                        }
                        TERM_ADVANCE(5);
                        #endif
                    } else {
                        if (c == 'K') {
                            int16_t original_cursor_x = self->cursor_x;
                            int16_t original_cursor_y = self->cursor_y;
                            int16_t clr_start = self->cursor_x;
                            int16_t clr_end = self->scroll_area->width_in_tiles;
                            #if CIRCUITPY_TERMINALIO_VT100
                            if (vt_args[0] == 1) {
                                clr_start = 0;
                                clr_end = self->cursor_x;
                            } else if (vt_args[0] == 2) {
                                clr_start = 0;
                            }
                            self->cursor_x = clr_start;
                            #endif
                            // Clear the (start/rest/all) of the line.
                            for (uint16_t k = clr_start; k < clr_end; k++) {
                                terminalio_terminal_set_tile(self, false, ' ', true);
                            }
                            self->cursor_x = original_cursor_x;
                            self->cursor_y = original_cursor_y;
                        } else if (c == 'D') {
                            if (vt_args[0] > self->cursor_x) {
                                self->cursor_x = 0;
                            } else {
                                self->cursor_x -= vt_args[0];
                            }
                        } else if (c == 'J') {
                            if (vt_args[0] == 2) {
                                common_hal_displayio_tilegrid_set_top_left(self->scroll_area, 0, 0);
                                self->cursor_x = self->cursor_y = start_y = 0;
                                terminalio_terminal_set_all_tiles(self, false, ' ', true);
                            }
                        } else if (c == 'H') {
                            if (vt_args[0] > 0) {
                                vt_args[0]--;
                            }
                            if (vt_args[1] > 0) {
                                vt_args[1]--;
                            }
                            if (vt_args[0] >= self->scroll_area->height_in_tiles) {
                                vt_args[0] = self->scroll_area->height_in_tiles - 1;
                            }
                            if (vt_args[1] >= self->scroll_area->width_in_tiles) {
                                vt_args[1] = self->scroll_area->width_in_tiles - 1;
                            }
                            vt_args[0] = SCRNMOD(vt_args[0]);
                            self->cursor_x = vt_args[1];
                            self->cursor_y = vt_args[0];
                            start_y = self->cursor_y;
                        #if CIRCUITPY_TERMINALIO_VT100
                        } else if (c == 'm') {
                            for (uint8_t i_args = 0; i_args < n_args; i_args++) {
                                if ((vt_args[i_args] >= 40 && vt_args[i_args] <= 47) || (vt_args[i_args] >= 30 && vt_args[i_args] <= 37)) {
                                    common_hal_displayio_palette_set_color(terminal_palette, 1 - (vt_args[i_args] / 40), _select_color(vt_args[i_args] % 10));
                                }
                                if (vt_args[i_args] == 0) {
                                    common_hal_displayio_palette_set_color(terminal_palette, 0, 0x000000);
                                    common_hal_displayio_palette_set_color(terminal_palette, 1, 0xffffff);
                                }
                            }
                        } else if (c == 'r') {
                            if (vt_args[0] < vt_args[1] && vt_args[0] >= 1 && vt_args[1] <= self->scroll_area->height_in_tiles) {
                                self->vt_scroll_top = vt_args[0] - 1;
                                self->vt_scroll_end = vt_args[1] - 1;
                            } else {
                                self->vt_scroll_top = 0;
                                self->vt_scroll_end = self->scroll_area->height_in_tiles - 1;
                            }
                            self->cursor_x = 0;
                            self->cursor_y = self->scroll_area->top_left_y % self->scroll_area->height_in_tiles;
                            start_y = self->cursor_y;
                        #endif
                        }
                        TERM_ADVANCE(j + 1);
                    }
                #if CIRCUITPY_TERMINALIO_VT100
                } else if (TERM_PEEK(0) == 'M') {
                    if (self->cursor_y != SCRNMOD(self->vt_scroll_top)) {
                        if (self->cursor_y > 0) {
                            self->cursor_y = self->cursor_y - 1;
                        } else {
                            self->cursor_y = self->scroll_area->height_in_tiles - 1;
                        }
                    } else {
                        if (self->vt_scroll_top != 0 || self->vt_scroll_end != self->scroll_area->height_in_tiles - 1) {
                            // Scroll range defined, manually move tiles to perform scroll
                            for (int16_t irow = self->vt_scroll_end - 1; irow >= self->vt_scroll_top; irow--) {
                                for (int16_t icol = 0; icol < self->scroll_area->width_in_tiles; icol++) {
                                    common_hal_displayio_tilegrid_set_tile(self->scroll_area, icol, SCRNMOD(irow + 1), common_hal_displayio_tilegrid_get_tile(self->scroll_area, icol, SCRNMOD(irow)));
                                }
                            }
                            self->cursor_x = 0;
                            int16_t old_y = self->cursor_y;
                            // Fill the row with spaces.
                            for (int16_t icol = 0; icol < self->scroll_area->width_in_tiles; icol++) {
                                terminalio_terminal_set_tile(self, false, ' ', true);
                            }
                            self->cursor_y = old_y;
                        } else {
                            // Full screen scroll, just set new top_y pointer and clear row
                            if (self->cursor_y > 0) {
                                common_hal_displayio_tilegrid_set_top_left(self->scroll_area, 0, self->cursor_y - 1);
                            } else {
                                common_hal_displayio_tilegrid_set_top_left(self->scroll_area, 0, self->scroll_area->height_in_tiles - 1);
                            }

                            self->cursor_x = 0;
                            self->cursor_y = self->scroll_area->top_left_y;
                            // Fill the row with spaces.
                            for (int16_t icol = 0; icol < self->scroll_area->width_in_tiles; icol++) {
                                terminalio_terminal_set_tile(self, false, ' ', true);
                            }
                            self->cursor_y = self->scroll_area->top_left_y;
                        }
                        self->cursor_x = 0;
                    }
                    start_y = self->cursor_y;
                    i++;
                } else if (TERM_PEEK(0) == 'D') {
                    self->cursor_y++;
                    i++;
                #endif
                } else if (TERM_PEEK(0) == ']' && c == ';') {
                    self->in_osc_command = true;
                    self->osc_command = vt_args[0];
                    TERM_ADVANCE(j + 1);
                }
            }
        } else {
            terminalio_terminal_set_tile(self, false, c, true);
        }
        if (self->cursor_x >= self->scroll_area->width_in_tiles) {
            self->cursor_y++;
            self->cursor_x %= self->scroll_area->width_in_tiles;
        }
        if (self->cursor_y >= self->scroll_area->height_in_tiles) {
            self->cursor_y %= self->scroll_area->height_in_tiles;
        }
        if (self->cursor_y != start_y) {
            if (((self->cursor_y + self->scroll_area->height_in_tiles) - 1) % self->scroll_area->height_in_tiles == SCRNMOD(self->vt_scroll_end)) {
                #if CIRCUITPY_TERMINALIO_VT100
                if (self->vt_scroll_top != 0 || self->vt_scroll_end != self->scroll_area->height_in_tiles - 1) {
                    // Scroll range defined, manually move tiles to perform scroll
                    self->cursor_y = SCRNMOD(self->vt_scroll_end);

                    for (int16_t irow = self->vt_scroll_top; irow < self->vt_scroll_end; irow++) {
                        for (int16_t icol = 0; icol < self->scroll_area->width_in_tiles; icol++) {
                            common_hal_displayio_tilegrid_set_tile(self->scroll_area, icol, SCRNMOD(irow), common_hal_displayio_tilegrid_get_tile(self->scroll_area, icol, SCRNMOD(irow + 1)));
                        }
                    }
                }
                #endif
                if (self->vt_scroll_top == 0 && self->vt_scroll_end == self->scroll_area->height_in_tiles - 1) {
                    // Full screen scroll, just set new top_y pointer
                    common_hal_displayio_tilegrid_set_top_left(self->scroll_area, 0, (self->cursor_y + self->scroll_area->height_in_tiles + 1) % self->scroll_area->height_in_tiles);
                }
                // clear the new row in case of scroll up
                self->cursor_x = 0;
                int16_t old_y = self->cursor_y;
                for (int16_t icol = 0; icol < self->scroll_area->width_in_tiles; icol++) {
                    terminalio_terminal_set_tile(self, false, ' ', true);
                }
                self->cursor_x = 0;
                self->cursor_y = old_y;
            }
            start_y = self->cursor_y;
        }
    }
    #undef TERM_PEEK
    #undef TERM_ADVANCE
    // A truncated escape sequence still advances i by the length it would have had,
    // which can land past the end. Report what we were actually given.
    return MIN((size_t)(i - data), len);
}

uint16_t common_hal_terminalio_terminal_get_cursor_x(terminalio_terminal_obj_t *self) {
    return self->cursor_x;
}
uint16_t common_hal_terminalio_terminal_get_cursor_y(terminalio_terminal_obj_t *self) {
    return self->cursor_y;
}

bool common_hal_terminalio_terminal_ready_to_tx(terminalio_terminal_obj_t *self) {
    return self->scroll_area != NULL;
}
