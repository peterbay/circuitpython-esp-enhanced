// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (C) 2026 ESPsoup contributors / Peter Holzhauser
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: GPL-3.0-or-later

// Fixed-point signal processing for espsdr, included by Receiver.c only. The
// FFT, the sine table and the dB conversion are taken from the ESPsoup firmware
// (https://github.com/pit711/espsoup-firmware, main/families/c5_c6_c61/c5_dsp.h,
// GPL-3.0-or-later); everything is integer, the ESP32-C5 having no FPU.

#pragma once

#include <math.h>
#include <stdint.h>

// Q14 sine over a full turn; cos(x) is sin_tab[(x + 256) & 1023].
static int16_t sin_tab[1024];

static void dsp_init_tables(void) {
    static bool done;
    if (done) {
        return;
    }
    for (unsigned j = 0; j < 1024; j++) {
        sin_tab[j] = (int16_t)lroundf(16383.0f * sinf(2.0f * (float)M_PI * j / 1024.0f));
    }
    done = true;
}

// In-place radix-2 complex FFT of n (a power of two, at most 1024: the
// table's resolution) int32 I, Q pairs, Q14 twiddles from sin_tab.
#define FFT_MAX (1024)
static void fft_i32(int32_t *x, unsigned n) {
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            int32_t t = x[2 * i];
            x[2 * i] = x[2 * j];
            x[2 * j] = t;
            t = x[2 * i + 1];
            x[2 * i + 1] = x[2 * j + 1];
            x[2 * j + 1] = t;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        unsigned step = 1024 / len;
        unsigned half = len / 2;
        for (unsigned i = 0; i < n; i += len) {
            for (unsigned k = 0; k < half; k++) {
                int32_t c = sin_tab[(k * step + 256) & 1023], s = -sin_tab[(k * step) & 1023];
                int32_t *a = x + 2 * (i + k), *b = x + 2 * (i + k + half);
                int32_t br = (int32_t)(((int64_t)b[0] * c - (int64_t)b[1] * s) >> 14);
                int32_t bi = (int32_t)(((int64_t)b[0] * s + (int64_t)b[1] * c) >> 14);
                b[0] = a[0] - br;
                b[1] = a[1] - bi;
                a[0] += br;
                a[1] += bi;
            }
        }
    }
}

// 100 * 10 * log10(v) for v > 0: log2 from the leading zeros and a 64-entry
// table of the mantissa.
static int32_t db100_u64(uint64_t v) {
    static int16_t lt[65];
    static bool init;
    if (v == 0) {
        return -10000;
    }
    if (!init) {
        for (int j = 0; j <= 64; j++) {
            lt[j] = (int16_t)lroundf(30103.0f * log2f(1 + j / 64.0f));
        }
        init = true;
    }
    int e = 63 - __builtin_clzll(v);
    unsigned frac = e >= 6 ? (unsigned)((v >> (e - 6)) & 63) : (unsigned)((v << (6 - e)) & 63);
    return (int32_t)((e * 30103 + lt[frac]) / 100);
}

// The 10-bit fields of a dump word, sign-extended: I in bits 10-19, Q in 0-9
// (see capture() for why that way round).
static inline int32_t word_i(uint32_t w) {
    return (int32_t)(w << 12) >> 22;
}

static inline int32_t word_q(uint32_t w) {
    return (int32_t)(w << 22) >> 22;
}

// The sum over navg blocks of nfft words each of |FFT|^2, after the mean of the
// whole capture is taken off and a Hann window applied; bin k of acc is
// frequency k * rate / nfft, so negative frequencies are in the upper half.
// x is a workspace of 2 * nfft int32.
static void power_spectrum(const volatile uint32_t *words, unsigned nfft, unsigned navg, int32_t *x, uint64_t *acc) {
    unsigned n = nfft * navg;
    int64_t si = 0, sq = 0;
    for (unsigned j = 0; j < n; j++) {
        si += word_i(words[j]);
        sq += word_q(words[j]);
    }
    // Inputs are scaled by 16 so that the mean comes off in fractions.
    int32_t mi = (int32_t)(si * 16 / (int64_t)n), mq = (int32_t)(sq * 16 / (int64_t)n);
    for (unsigned j = 0; j < nfft; j++) {
        acc[j] = 0;
    }
    for (unsigned a = 0; a < navg; a++) {
        const volatile uint32_t *w = words + a * nfft;
        for (unsigned j = 0; j < nfft; j++) {
            // Hann, 0..32767.
            int32_t win = 16384 - sin_tab[((j * 1024) / nfft + 256) & 1023];
            int32_t i = word_i(w[j]) * 16 - mi, q = word_q(w[j]) * 16 - mq;
            x[2 * j] = (i * win) >> 15;
            x[2 * j + 1] = (q * win) >> 15;
        }
        fft_i32(x, nfft);
        for (unsigned j = 0; j < nfft; j++) {
            acc[j] += (uint64_t)((int64_t)x[2 * j] * x[2 * j]) + (uint64_t)((int64_t)x[2 * j + 1] * x[2 * j + 1]);
        }
    }
}

// Kaiser-windowed low-pass for decimation by r: 16 * r + 1 taps (at most
// FIR_MAX), cutoff half the output rate, beta 7, Q15 with unity gain at DC. The
// band within about +-38 % of the output rate comes out flat and alias-free.
// Designed in float once per r and kept.
#define FIR_MAX (1025)
// Above 64 the taps are capped, and the filter is less steep than it could be.
#define DECIMATION_MAX (128)

static int16_t fir_h[FIR_MAX];
static uint16_t fir_nz[FIR_MAX / 2 + 1];
static unsigned fir_taps, fir_nnz, fir_r;

static unsigned fir_taps_for(unsigned r) {
    unsigned taps = 16 * r + 1;
    return taps > FIR_MAX ? FIR_MAX : taps;
}

static float bessel_i0(float x) {
    float s = 1, t = 1;
    for (int k = 1; k < 25; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

static void fir_design(unsigned r) {
    if (fir_r == r) {
        return;
    }
    const unsigned taps = fir_taps_for(r);
    const float fc = 0.5f / r, beta = 7.0f;
    // Two passes, the sum and then the scaled taps, instead of a float buffer.
    float sum = 0, m = (taps - 1) / 2.0f, i0b = bessel_i0(beta);
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned j = 0; j < taps; j++) {
            float x = j - m;
            float s = j == (taps - 1) / 2 ? 2 * fc : sinf(2 * (float)M_PI * fc * x) / ((float)M_PI * x);
            float q = x / m;
            float win = bessel_i0(beta * sqrtf(fmaxf(0, 1 - q * q))) / i0b;
            if (pass) {
                fir_h[j] = (int16_t)lroundf(s * win / sum * 32767.0f);
            } else {
                sum += s * win;
            }
        }
    }
    fir_taps = taps;
    // The filter is symmetric: only the non-zero taps before the centre.
    fir_nnz = 0;
    for (unsigned j = 0; j < (taps - 1) / 2; j++) {
        if (fir_h[j] != 0) {
            fir_nz[fir_nnz++] = j;
        }
    }
    fir_r = r;
}

// Raw words in, n_out decimated I, Q int16 pairs out, in place: need =
// n_out * r + fir_taps_for(r) words. The pairs are in units of 1/64 of the
// 10-bit LSB, the mean of the capture taken off.
static int16_t *decimate(volatile uint32_t *words, size_t n_out, unsigned r) {
    fir_design(r);
    const unsigned taps = fir_taps;
    const size_t need = n_out * r + taps;
    int64_t si = 0, sq = 0;
    for (size_t j = 0; j < need; j++) {
        si += word_i(words[j]);
        sq += word_q(words[j]);
    }
    int32_t mi = (int32_t)(si * 64 / (int64_t)need), mq = (int32_t)(sq * 64 / (int64_t)need);
    for (size_t j = 0; j < need; j++) {
        uint32_t w = words[j];
        int32_t i = word_i(w) * 64 - mi, q = word_q(w) * 64 - mq;
        i = i > 32767 ? 32767 : i < -32768 ? -32768 : i;
        q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
        words[j] = (uint16_t)(int16_t)i | ((uint32_t)(uint16_t)(int16_t)q << 16);
    }
    // Output j never overwrites an input still needed: those start at word j * r.
    int16_t *in = (int16_t *)words;
    const unsigned centre = (taps - 1) / 2;
    for (size_t j = 0; j < n_out; j++) {
        const int16_t *p = in + 2 * (j * r);
        int32_t acc_i = p[2 * centre] * fir_h[centre], acc_q = p[2 * centre + 1] * fir_h[centre];
        for (unsigned z = 0; z < fir_nnz; z++) {
            unsigned t = fir_nz[z], u = taps - 1 - t;
            int32_t h = fir_h[t];
            acc_i += (p[2 * t] + p[2 * u]) * h;
            acc_q += (p[2 * t + 1] + p[2 * u + 1]) * h;
        }
        in[2 * j] = acc_i >> 15;
        in[2 * j + 1] = acc_q >> 15;
    }
    return in;
}

// The level of a full-scale tone (amplitude 512, scaled by 16, Hann's
// coherent gain 1/2) in one bin, in the units of db100_u64().
static int32_t full_scale_db100(unsigned nfft) {
    uint64_t peak = (uint64_t)512 * 16 * nfft / 2;
    return db100_u64(peak * peak);
}
