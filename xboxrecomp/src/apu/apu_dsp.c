/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 *
 * The address is not derivable from the APU registers: GPSADDR/GPFADDR/
 * EPSADDR/EPFADDR point at the DSP's own scratch and frame memory, while the
 * command block is a DirectSound heap allocation. On Wreckless the registers
 * read 0x01504000 / 0x014EC000 / 0x0151C000 / 0x014F0000 and the doorbell is at
 * 0x014F8810 -- inside none of them. So it has to be observed: run with
 * RECOMP_WATCHDOG_SECS and the spin shows up as ebx plus the poll offset.
 */
#define APU_DSP_ACK_MAX 8
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        return;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
static int mcpx_apu_mixdown_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN_ALL");
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on;
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

/* ── Stereo mixdown standing in for the GP/EP ─────────────────────────────
 *
 * The VP leaves 32 mixbins; on hardware the GP's DSP program (DirectSound's
 * standard effects image) turns them into speaker feeds and the EP encodes
 * those. Neither runs here, so this folds the bins down to stereo the way
 * the standard image routes them:
 *
 *   0 front L, 1 front R, 2 centre, 3 LFE, 4 back L, 5 back R,
 *   6..9 the crosstalk (virtual-surround) FL/FR/BL/BR, 10 the I3DL2 reverb
 *   send, 11..30 effect sends, 31 the multipass bin.
 *
 * The multipass bin is an intermediate: multipass voices read it back and
 * redistribute it, so playing it as well doubled every sound routed through
 * it -- RalliSport Challenge 2 sends its engine and most effects there.
 * The reverb send is the input of a reverb, not something to hear dry; it
 * feeds a small reverb below. The previous even-left / odd-right split
 * played bin 31 dry on the right and bin 10 dry on the left, which with
 * everything summed hard-clipped about one sample in a hundred in a race.
 *
 * Bins come back up by the headroom the VP divided them by (the GP program
 * undoes it), and the result goes through a soft limiter instead of being
 * hard-clipped.
 *
 * ponytail: not the title's DSP program -- no per-title effects, and the
 * reverb is a fixed Freeverb, not the I3DL2 parameters the title set.
 * RECOMP_APU_MIXDOWN_ALL=0 restores the old two-bin read, RECOMP_APU_REVERB=<wet>
 * sets the reverb level (0 turns it off). */

#define FV_COMBS 8
#define FV_APS   4
static const int fv_comb_len[FV_COMBS] = { 1215, 1293, 1390, 1476, 1548, 1623, 1695, 1760 };
static const int fv_ap_len[FV_APS] = { 605, 480, 371, 245 };
#define FV_SPREAD 25

typedef struct {
    float *comb[2][FV_COMBS], comb_lp[2][FV_COMBS];
    int comb_i[2][FV_COMBS], comb_n[2][FV_COMBS];
    float *ap[2][FV_APS];
    int ap_i[2][FV_APS], ap_n[2][FV_APS];
} Reverb;

static Reverb s_rv;
static float s_rv_wet = -1.0f;

static void reverb_init(void)
{
    const char *e = getenv("RECOMP_APU_REVERB");
    int c, k;
    s_rv_wet = e && *e ? (float)atof(e) : 0.3f;
    if (s_rv_wet <= 0.0f)
        return;
    for (c = 0; c < 2; c++) {
        for (k = 0; k < FV_COMBS; k++) {
            s_rv.comb_n[c][k] = fv_comb_len[k] + c * FV_SPREAD;
            s_rv.comb[c][k] = (float *)calloc((size_t)s_rv.comb_n[c][k], sizeof(float));
        }
        for (k = 0; k < FV_APS; k++) {
            s_rv.ap_n[c][k] = fv_ap_len[k] + c * FV_SPREAD;
            s_rv.ap[c][k] = (float *)calloc((size_t)s_rv.ap_n[c][k], sizeof(float));
        }
    }
}

/* One mono sample in, a stereo pair of reverb out (Freeverb structure). */
static void reverb_tick(float in, float *outl, float *outr)
{
    const float feedback = 0.80f, damp = 0.25f;
    int c, k;
    in *= 0.015f;
    for (c = 0; c < 2; c++) {
        float acc = 0.0f;
        for (k = 0; k < FV_COMBS; k++) {
            float *b = s_rv.comb[c][k];
            int i = s_rv.comb_i[c][k];
            float y = b[i];
            s_rv.comb_lp[c][k] = y * (1.0f - damp) + s_rv.comb_lp[c][k] * damp;
            b[i] = in + s_rv.comb_lp[c][k] * feedback;
            if (++i >= s_rv.comb_n[c][k]) i = 0;
            s_rv.comb_i[c][k] = i;
            acc += y;
        }
        for (k = 0; k < FV_APS; k++) {
            float *b = s_rv.ap[c][k];
            int i = s_rv.ap_i[c][k];
            float bo = b[i];
            b[i] = acc + bo * 0.5f;
            acc = bo - acc;
            if (++i >= s_rv.ap_n[c][k]) i = 0;
            s_rv.ap_i[c][k] = i;
        }
        if (c == 0) *outl = acc * 3.0f;
        else        *outr = acc * 3.0f;
    }
}

/* Unity below 0.75, then a tanh knee that never quite reaches 1. */
static inline float soft_limit(float x)
{
    float a = fabsf(x);
    if (a <= 0.75f)
        return x;
    a = 0.75f + 0.25f * tanhf((a - 0.75f) / 0.25f);
    return x < 0.0f ? -a : a;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (s_rv_wet < 0.0f)
        reverb_init();

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        float gain[NUM_MIXBINS];
        for (int b = 0; b < NUM_MIXBINS; b++)
            gain[b] = (float)(1 << d->vp.submix_headroom[b]);
        {
            static unsigned n;
            if (getenv("RECOMP_APU_STATS") && (n++ % 7500) == 0) {
                fprintf(stderr, "[APU-STATS] headroom: hrtf %u, bins", d->vp.hrtf_headroom);
                for (int b = 0; b < NUM_MIXBINS; b++)
                    fprintf(stderr, " %u", d->vp.submix_headroom[b]);
                fputc('\n', stderr);
            }
        }
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float left, right;
            if (mcpx_apu_mixdown_all()) {
                float c = 0.7071f * (mixbins[2][i] * gain[2] + mixbins[3][i] * gain[3]);
                float fx = 0.0f;
                left  = mixbins[0][i] * gain[0] + mixbins[4][i] * gain[4]
                      + mixbins[6][i] * gain[6] + mixbins[8][i] * gain[8] + c;
                right = mixbins[1][i] * gain[1] + mixbins[5][i] * gain[5]
                      + mixbins[7][i] * gain[7] + mixbins[9][i] * gain[9] + c;
                for (int b = 11; b < MULTIPASS_BIN; b++)
                    fx += mixbins[b][i] * gain[b];
                left += 0.7071f * fx;
                right += 0.7071f * fx;
                if (s_rv_wet > 0.0f) {
                    float rl, rr;
                    reverb_tick(mixbins[10][i] * gain[10], &rl, &rr);
                    left += s_rv_wet * rl;
                    right += s_rv_wet * rr;
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            left = soft_limit(left);
            right = soft_limit(right);

            /* Write (not accumulate) into the frame buffer: each of the 8
             * sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
