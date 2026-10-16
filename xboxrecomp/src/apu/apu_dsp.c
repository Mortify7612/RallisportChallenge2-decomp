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
#include <stdio.h>
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
 * The reverb follows the title's I3DL2 listener (mcpx_apu_set_i3dl2, from
 * DirectSound's SetI3DL2Listener), which it changes as the car moves between
 * environments. On the console it is the reverb in the GP's DSP program;
 * here an I3DL2 reverb in C stands in for it: the room's HF cut on the way
 * in, early reflections from a tapped delay at the reflections delay and
 * level, and a late reverb -- input diffusers, then an eight-line feedback
 * delay network -- at the reverb delay and level, decaying in the title's
 * decay time, its HF part in decay time x HF ratio. The levels follow the
 * EAX/I3DL2 reading OpenAL Soft uses (reflections and reverb relative to
 * room, the late reverb's energy normalised for its decay). Not the DSP
 * program's exact sound, but its parameters.
 *
 * RECOMP_APU_MIXDOWN_ALL=0 restores the old two-bin read. RECOMP_APU_REVERB
 * scales the reverb: 0.3 (the launcher's Normal) is the title's level, 0 is
 * off. RECOMP_APU_I3DL2_TRACE prints each listener the title sets. */

#define RV_RATE     48000.0f
#define RV_LINES    8
#define RV_DIFF     4
#define RV_PRE_MAX  32768           /* > 0.3 s + 0.1 s + taps at 48 kHz */
#define RV_LINE_MAX 8192
#define RV_TAPS     6

static const float rv_line_ms[RV_LINES] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.1f, 61.3f, 67.9f, 73.3f };
static const float rv_diff_ms[RV_DIFF] = { 4.77f, 3.59f, 2.73f, 1.73f };
static const float rv_tap_ms[RV_TAPS] = { 0.0f, 3.1f, 5.3f, 7.7f, 11.2f, 13.9f };
static const float rv_tap_g[RV_TAPS] = { 1.0f, 0.83f, 0.71f, 0.62f, 0.52f, 0.45f };

/* What the parameters become; `cur` glides to `want` (no zipper noise when
 * the environment changes). */
typedef struct {
    float in_lp;                /* room HF: one-pole coefficient */
    float refl, late;           /* output levels */
    float fb[RV_LINES];         /* per-line decay gain */
    float damp[RV_LINES];       /* per-line HF damping coefficient */
    float diff;                 /* diffuser allpass coefficient */
    float refl_delay, late_delay;   /* samples */
} RvCoef;

typedef struct {
    float pre[RV_PRE_MAX];
    int pre_i;
    float line[RV_LINES][RV_LINE_MAX];
    int line_n[RV_LINES], line_i[RV_LINES];
    float line_lp[RV_LINES];
    float diff[RV_DIFF][1024];
    int diff_n[RV_DIFF], diff_i[RV_DIFF];
    float in_lp_z;
    RvCoef cur, want;
    volatile int pending;       /* a new listener waits in `next` */
    uint32_t next[12];
} Reverb;

static Reverb *s_rv;
static float s_rv_wet = -1.0f;
/* RECOMP_APU_REVERB_STATS: once a second, the dry mix, the I3DL2 send and
 * the reverb's output, in dB. */
static int s_rv_stats;
static double s_rv_e[3];
static int s_rv_n;

static float mb_gain(float mb) { return powf(10.0f, mb / 2000.0f); }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* One-pole lowpass y = (1-a) x + a y with gain `g` (< 1) at `hz` (OpenAL
 * Soft's lpCoeffCalc). */
static float lp_coef(float g, float hz)
{
    float w = 2.0f * 3.14159265f * hz / RV_RATE, cw = cosf(w), gs, a;
    if (g >= 0.9999f)
        return 0.0f;
    if (g < 0.001f)
        g = 0.001f;
    gs = g * g;
    a = (1.0f - gs * cw - sqrtf(2.0f * gs * (1.0f - cw) - gs * gs * (1.0f - cw * cw))) / (1.0f - gs);
    return a < 0.0f ? 0.0f : a > 0.99f ? 0.99f : a;
}

static void rv_coefs(const uint32_t p[12], RvCoef *c)
{
    float f[12];
    /* I3DL2's ranges: the title's blends between environments can step
     * past them. */
    int32_t room = clampi((int32_t)p[0], -10000, 0), room_hf = clampi((int32_t)p[1], -10000, 0);
    int32_t refl = clampi((int32_t)p[5], -10000, 1000), rev = clampi((int32_t)p[7], -10000, 2000);
    float decay, hf_ratio, hf_ref, density, diffusion;
    int k;
    memcpy(f, p, sizeof f);
    decay = f[3] < 0.1f ? 0.1f : f[3] > 20.0f ? 20.0f : f[3];
    hf_ratio = f[4] < 0.1f ? 0.1f : f[4] > 2.0f ? 2.0f : f[4];
    hf_ref = f[11] < 20.0f ? 5000.0f : f[11] > 20000.0f ? 20000.0f : f[11];
    diffusion = f[9] < 0.0f ? 0.0f : f[9] > 100.0f ? 1.0f : f[9] / 100.0f;
    density = f[10] < 0.0f ? 0.0f : f[10] > 100.0f ? 1.0f : f[10] / 100.0f;
    (void)density;              /* the line lengths stay put: no clicks */
    c->in_lp = lp_coef(mb_gain((float)room_hf), hf_ref);
    c->refl = room <= -10000 ? 0.0f : mb_gain((float)(room + refl));
    c->late = room <= -10000 ? 0.0f : mb_gain((float)(room + rev));
    c->diff = 0.6f * diffusion;
    c->refl_delay = (f[6] < 0.0f ? 0.0f : f[6] > 0.3f ? 0.3f : f[6]) * RV_RATE;
    c->late_delay = c->refl_delay + (f[8] < 0.0f ? 0.0f : f[8] > 0.1f ? 0.1f : f[8]) * RV_RATE;
    for (k = 0; k < RV_LINES; k++) {
        float len = rv_line_ms[k] * RV_RATE / 1000.0f;
        float g = powf(10.0f, -3.0f * len / (decay * RV_RATE));
        float ghf = powf(10.0f, -3.0f * len / (decay * hf_ratio * RV_RATE));
        c->fb[k] = g;
        c->damp[k] = ghf < g ? lp_coef(ghf / g, hf_ref) : 0.0f;
    }
    /* The late tail's energy, 1 / (1 - g^2), normalised away so lReverb is
     * its level whatever the decay. */
    {
        float gm = 0.0f;
        for (k = 0; k < RV_LINES; k++)
            gm += c->fb[k] * c->fb[k];
        gm /= RV_LINES;
        c->late *= sqrtf(1.0f - gm);
    }
}

void mcpx_apu_set_i3dl2(const uint32_t p[12])
{
    static uint32_t last[12];
    if (!memcmp(last, p, sizeof last))
        return;
    memcpy(last, p, sizeof last);
    if (getenv("RECOMP_APU_I3DL2_TRACE")) {
        float f[12];
        memcpy(f, p, sizeof f);
        fprintf(stderr, "[I3DL2] room %d hf %d rolloff %.2f decay %.2f hfratio %.2f refl %d @%.3f"
                " reverb %d @%.3f diffusion %.0f density %.0f hfref %.0f\n",
                (int32_t)p[0], (int32_t)p[1], f[2], f[3], f[4], (int32_t)p[5], f[6],
                (int32_t)p[7], f[8], f[9], f[10], f[11]);
    }
    if (s_rv) {
        memcpy(s_rv->next, p, sizeof s_rv->next);
        __atomic_store_n(&s_rv->pending, 1, __ATOMIC_RELEASE);
    }
}

static void reverb_init(void)
{
    /* DirectSound's default listener (its Generic preset) until the title
     * sets one. */
    static const uint32_t generic[12] = {
        (uint32_t)-1000, (uint32_t)-100, 0x00000000u, 0x3FBEB852u /* 1.49 */,
        0x3F547AE1u /* 0.83 */, (uint32_t)-2602, 0x3BE56042u /* 0.007 */, 200u,
        0x3C343958u /* 0.011 */, 0x42C80000u /* 100 */, 0x42C80000u, 0x459C4000u /* 5000 */ };
    const char *e = getenv("RECOMP_APU_REVERB");
    int k;
    s_rv_wet = e && *e ? (float)atof(e) / 0.3f : 1.0f;
    s_rv_stats = getenv("RECOMP_APU_REVERB_STATS") != NULL;
    if (s_rv_wet <= 0.0f)
        return;
    s_rv = (Reverb *)calloc(1, sizeof *s_rv);
    if (!s_rv) {
        s_rv_wet = 0.0f;
        return;
    }
    for (k = 0; k < RV_LINES; k++)
        s_rv->line_n[k] = (int)(rv_line_ms[k] * RV_RATE / 1000.0f);
    for (k = 0; k < RV_DIFF; k++)
        s_rv->diff_n[k] = (int)(rv_diff_ms[k] * RV_RATE / 1000.0f);
    rv_coefs(generic, &s_rv->want);
    s_rv->cur = s_rv->want;
}

static inline float rv_pre_at(const Reverb *r, float d)
{
    int i = r->pre_i - 1 - (int)d;
    while (i < 0)
        i += RV_PRE_MAX;
    return r->pre[i];
}

/* One mono sample of the I3DL2 send in, a stereo pair of reverb out. */
static void reverb_tick(float in, float *outl, float *outr)
{
    Reverb *r = s_rv;
    RvCoef *c = &r->cur;
    const float glide = 0.002f;         /* ~10 ms */
    float x, el = 0.0f, er = 0.0f, ll = 0.0f, lr = 0.0f, sum = 0.0f;
    float out[RV_LINES];
    int k;

    if (__atomic_load_n(&r->pending, __ATOMIC_ACQUIRE)) {
        uint32_t p[12];
        memcpy(p, r->next, sizeof p);
        __atomic_store_n(&r->pending, 0, __ATOMIC_RELEASE);
        rv_coefs(p, &r->want);
    }
    c->in_lp += (r->want.in_lp - c->in_lp) * glide;
    c->refl += (r->want.refl - c->refl) * glide;
    c->late += (r->want.late - c->late) * glide;
    c->diff += (r->want.diff - c->diff) * glide;
    c->refl_delay += (r->want.refl_delay - c->refl_delay) * glide;
    c->late_delay += (r->want.late_delay - c->late_delay) * glide;
    for (k = 0; k < RV_LINES; k++) {
        c->fb[k] += (r->want.fb[k] - c->fb[k]) * glide;
        c->damp[k] += (r->want.damp[k] - c->damp[k]) * glide;
    }

    /* Room HF on the way in, then the shared pre-delay. */
    r->in_lp_z = (1.0f - c->in_lp) * in + c->in_lp * r->in_lp_z;
    r->pre[r->pre_i] = r->in_lp_z;
    if (++r->pre_i >= RV_PRE_MAX)
        r->pre_i = 0;

    /* Early reflections: taps after the reflections delay, alternating. */
    for (k = 0; k < RV_TAPS; k++) {
        float v = rv_pre_at(r, c->refl_delay + rv_tap_ms[k] * RV_RATE / 1000.0f) * rv_tap_g[k];
        if (k & 1) er += v; else el += v;
    }
    el *= c->refl * 0.6f;
    er *= c->refl * 0.6f;

    /* Late reverb: diffusers, then the feedback delay network. */
    x = rv_pre_at(r, c->late_delay);
    for (k = 0; k < RV_DIFF; k++) {
        float *b = r->diff[k];
        int i = r->diff_i[k];
        float bo = b[i], v = x + c->diff * bo;
        b[i] = v;
        x = bo - c->diff * v;
        if (++i >= r->diff_n[k]) i = 0;
        r->diff_i[k] = i;
    }
    for (k = 0; k < RV_LINES; k++) {
        float y = r->line[k][r->line_i[k]];
        r->line_lp[k] = (1.0f - c->damp[k]) * y + c->damp[k] * r->line_lp[k];
        out[k] = r->line_lp[k] * c->fb[k];
        sum += out[k];
    }
    sum *= 2.0f / RV_LINES;             /* Householder: I - 2/N 11^T */
    for (k = 0; k < RV_LINES; k++) {
        int i = r->line_i[k];
        r->line[k][i] = out[k] - sum + x;
        if (++i >= r->line_n[k]) i = 0;
        r->line_i[k] = i;
        if (k & 1) lr += out[k] * ((k & 2) ? -1.0f : 1.0f);
        else       ll += out[k] * ((k & 4) ? -1.0f : 1.0f);
    }
    ll *= c->late * 0.5f;
    lr *= c->late * 0.5f;
    *outl = el + ll;
    *outr = er + lr;
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
                    if (s_rv_stats) {
                        s_rv_e[0] += (double)left * left + (double)right * right;
                        s_rv_e[1] += (double)mixbins[10][i] * gain[10] * mixbins[10][i] * gain[10];
                        s_rv_e[2] += (double)(s_rv_wet * rl) * (s_rv_wet * rl) + (double)(s_rv_wet * rr) * (s_rv_wet * rr);
                        if (++s_rv_n == 48000) {
                            fprintf(stderr, "[REVERB] dry %.1f dB, send %.1f dB, wet %.1f dB\n",
                                    10 * log10(s_rv_e[0] / 48000 + 1e-12), 10 * log10(s_rv_e[1] / 48000 + 1e-12),
                                    10 * log10(s_rv_e[2] / 48000 + 1e-12));
                            s_rv_e[0] = s_rv_e[1] = s_rv_e[2] = 0;
                            s_rv_n = 0;
                        }
                    }
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
