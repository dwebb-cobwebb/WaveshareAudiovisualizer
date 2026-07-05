#include "dsp/loudness.h"
#include "config.h"

#include <math.h>
#include <string.h>

// ===========================================================================
// EBU R128 / ITU-R BS.1770-4 loudness, stereo 48 kHz.
//
// Pipeline (per channel): K-weighting (shelf + RLB high-pass biquads) ->
// mean-square over 100 ms sub-blocks. Momentary = last 4 sub-blocks (400 ms),
// short-term = last 30 (3 s), both at a 10 Hz update rate (the 75% overlap
// required by the spec falls out of the sub-block scheme).
//
// Integrated loudness applies the two-stage gate (absolute -70 LUFS, then
// relative -10 LU) over all momentary blocks since reset, using a loudness
// histogram with per-bin power sums so no per-block history is stored.
// LRA (EBU Tech 3342) uses the same scheme over short-term blocks with a
// -20 LU relative gate and the 10th..95th percentile spread.
//
// True peak follows BS.1770 Annex 2: 4x oversampling via a polyphase
// windowed-sinc interpolator, max-hold in dBTP.
// ===========================================================================

#define SUB_BLOCK_FRAMES  (AV_SAMPLE_RATE_HZ / 10)   // 100 ms = 4800
#define MOM_SUBS   4     // 400 ms momentary
#define ST_SUBS    30    // 3 s short-term

// Histogram over [-70, +5) LUFS in 0.1 LU bins.
#define HIST_MIN   (-70.0f)
#define HIST_BINS  750
#define ABS_GATE_LUFS  (-70.0f)

#define SILENCE_LUFS   (-120.0f)

// ---------------------------------------------------------------------------
// K-weighting biquads (BS.1770-4 coefficients for 48 kHz)
// ---------------------------------------------------------------------------
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad_t;

static const float SHELF[5] = {
    1.53512485958697f, -2.69169618940638f, 1.19839281085285f,
    -1.69065929318241f, 0.73248077421585f };
static const float HIPASS[5] = {
    1.0f, -2.0f, 1.0f,
    -1.99004745483398f, 0.99007225036621f };

static inline float bq_run(biquad_t *q, float x) {
    // Transposed direct form II
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static void bq_init(biquad_t *q, const float c[5]) {
    q->b0 = c[0]; q->b1 = c[1]; q->b2 = c[2];
    q->a1 = c[3]; q->a2 = c[4];
    q->z1 = q->z2 = 0.0f;
}

// ---------------------------------------------------------------------------
// True peak: 4x polyphase interpolator, 12 taps per phase (48-tap sinc).
// ---------------------------------------------------------------------------
#define TP_PHASES  4
#define TP_TAPS    12
static float s_tp_coef[TP_PHASES][TP_TAPS];
static float s_tp_hist[AV_CHANNELS][TP_TAPS];
static uint8_t s_tp_pos[AV_CHANNELS];

static void tp_init(void) {
    // Windowed sinc, cutoff slightly below Nyquist of the base rate.
    const float fc = 0.45f;   // cycles/sample at the input rate
    const int N = TP_PHASES * TP_TAPS;
    for (int ph = 0; ph < TP_PHASES; ph++) {
        float sum = 0.0f;
        for (int t = 0; t < TP_TAPS; t++) {
            int n = t * TP_PHASES + ph;
            float x = (float)n - (float)(N - 1) / 2.0f;
            float arg = 2.0f * (float)M_PI * fc * x / (float)TP_PHASES;
            float sinc = (fabsf(arg) < 1e-6f) ? 1.0f : sinf(arg) / arg;
            float win = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / (N - 1));
            s_tp_coef[ph][t] = sinc * win;
            sum += s_tp_coef[ph][t];
        }
        // Normalize each phase to unity DC gain so dBTP is not under-read.
        for (int t = 0; t < TP_TAPS; t++) s_tp_coef[ph][t] /= sum;
    }
    memset(s_tp_hist, 0, sizeof(s_tp_hist));
    memset(s_tp_pos, 0, sizeof(s_tp_pos));
}

static inline float tp_push(int ch, float x, float cur_max) {
    // Insert sample into the channel's circular history, evaluate 4 phases.
    uint8_t pos = s_tp_pos[ch];
    s_tp_hist[ch][pos] = x;
    s_tp_pos[ch] = (uint8_t)((pos + 1) % TP_TAPS);
    for (int ph = 0; ph < TP_PHASES; ph++) {
        float acc = 0.0f;
        const float *c = s_tp_coef[ph];
        int idx = pos;
        for (int t = 0; t < TP_TAPS; t++) {
            acc += c[t] * s_tp_hist[ch][idx];
            idx = (idx == 0) ? TP_TAPS - 1 : idx - 1;
        }
        float a = fabsf(acc);
        if (a > cur_max) cur_max = a;
    }
    return cur_max;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static biquad_t s_shelf[AV_CHANNELS], s_hp[AV_CHANNELS];

static float    s_sub_acc;          // running sum of (kL^2 + kR^2) this sub-block
static uint32_t s_sub_n;            // frames accumulated
static float    s_subs[ST_SUBS];    // ring of sub-block mean powers
static uint32_t s_sub_head;         // next write index
static uint32_t s_sub_count;        // total sub-blocks seen (saturates at ST_SUBS)

// Gating histograms: counts + power sums per 0.1 LU bin.
static uint32_t s_hist_i_cnt[HIST_BINS];   // momentary blocks (integrated)
static float    s_hist_i_pow[HIST_BINS];
static uint32_t s_hist_s_cnt[HIST_BINS];   // short-term blocks (LRA)
static float    s_hist_s_pow[HIST_BINS];

static float s_tp_max;              // linear, max of both channels

static volatile uint32_t s_reset_req;
static uint32_t          s_reset_done;

static loudness_snapshot_t s_snap;

static inline float power_to_lufs(float p) {
    if (p < 1e-12f) return SILENCE_LUFS;
    return -0.691f + 10.0f * log10f(p);
}

static inline int lufs_bin(float l) {
    int b = (int)((l - HIST_MIN) * 10.0f);
    if (b < 0) b = -1;                    // below histogram: gated out anyway
    if (b >= HIST_BINS) b = HIST_BINS - 1;
    return b;
}

void loudness_init(void) {
    for (int ch = 0; ch < AV_CHANNELS; ch++) {
        bq_init(&s_shelf[ch], SHELF);
        bq_init(&s_hp[ch], HIPASS);
    }
    tp_init();
    s_sub_acc = 0.0f; s_sub_n = 0;
    memset(s_subs, 0, sizeof(s_subs));
    s_sub_head = s_sub_count = 0;
    memset(s_hist_i_cnt, 0, sizeof(s_hist_i_cnt));
    memset(s_hist_i_pow, 0, sizeof(s_hist_i_pow));
    memset(s_hist_s_cnt, 0, sizeof(s_hist_s_cnt));
    memset(s_hist_s_pow, 0, sizeof(s_hist_s_pow));
    s_tp_max = 0.0f;
    s_snap.lufs_m = s_snap.lufs_s = s_snap.lufs_i = SILENCE_LUFS;
    s_snap.lra = 0.0f;
    s_snap.tp_db = SILENCE_LUFS;
}

void loudness_request_reset(void) {
    s_reset_req++;
}

static void apply_reset(void) {
    memset(s_hist_i_cnt, 0, sizeof(s_hist_i_cnt));
    memset(s_hist_i_pow, 0, sizeof(s_hist_i_pow));
    memset(s_hist_s_cnt, 0, sizeof(s_hist_s_cnt));
    memset(s_hist_s_pow, 0, sizeof(s_hist_s_pow));
    s_tp_max = 0.0f;
    s_snap.lufs_i = SILENCE_LUFS;
    s_snap.lra = 0.0f;
    s_snap.tp_db = SILENCE_LUFS;
}

// Mean power of the last n sub-blocks.
static float recent_power(uint32_t n) {
    if (s_sub_count < n) return 0.0f;
    float acc = 0.0f;
    uint32_t idx = s_sub_head;   // one past the newest
    for (uint32_t i = 0; i < n; i++) {
        idx = (idx == 0) ? ST_SUBS - 1 : idx - 1;
        acc += s_subs[idx];
    }
    return acc / (float)n;
}

// Gated mean power from a histogram: first pass applies the absolute gate,
// second pass the relative gate at (first-pass loudness + rel_gate_lu).
static float gated_power(const uint32_t *cnt, const float *pow_,
                         float rel_gate_lu) {
    uint64_t n = 0; float p = 0.0f;
    for (int b = 0; b < HIST_BINS; b++) { n += cnt[b]; p += pow_[b]; }
    if (n == 0) return 0.0f;
    float thresh = power_to_lufs(p / (float)n) + rel_gate_lu;
    int b0 = lufs_bin(thresh);
    n = 0; p = 0.0f;
    for (int b = (b0 < 0 ? 0 : b0 + 1); b < HIST_BINS; b++) {
        n += cnt[b]; p += pow_[b];
    }
    if (n == 0) return 0.0f;
    return p / (float)n;
}

// LRA: 10th..95th percentile spread of short-term blocks above the -20 LU
// relative gate.
static float compute_lra(void) {
    float gp = gated_power(s_hist_s_cnt, s_hist_s_pow, 0.0f);
    // The relative gate for LRA is -20 LU below the *absolute-gated* mean.
    uint64_t n = 0; float p = 0.0f;
    for (int b = 0; b < HIST_BINS; b++) { n += s_hist_s_cnt[b]; p += s_hist_s_pow[b]; }
    if (n == 0) return 0.0f;
    (void)gp;
    float thresh = power_to_lufs(p / (float)n) - 20.0f;
    int b0 = lufs_bin(thresh);

    uint64_t total = 0;
    for (int b = (b0 < 0 ? 0 : b0 + 1); b < HIST_BINS; b++) total += s_hist_s_cnt[b];
    if (total < 2) return 0.0f;

    uint64_t lo_target = (uint64_t)(0.10f * (float)total);
    uint64_t hi_target = (uint64_t)(0.95f * (float)total);
    float lo = HIST_MIN, hi = HIST_MIN;
    uint64_t run = 0;
    for (int b = (b0 < 0 ? 0 : b0 + 1); b < HIST_BINS; b++) {
        run += s_hist_s_cnt[b];
        if (lo == HIST_MIN && run >= lo_target && s_hist_s_cnt[b])
            lo = HIST_MIN + (b + 0.5f) * 0.1f;
        if (run >= hi_target && s_hist_s_cnt[b]) {
            hi = HIST_MIN + (b + 0.5f) * 0.1f;
            break;
        }
    }
    float lra = hi - lo;
    return (lra > 0.0f) ? lra : 0.0f;
}

static void finish_sub_block(void) {
    float sub_power = s_sub_acc / (float)SUB_BLOCK_FRAMES;
    s_sub_acc = 0.0f; s_sub_n = 0;

    s_subs[s_sub_head] = sub_power;
    s_sub_head = (s_sub_head + 1) % ST_SUBS;
    if (s_sub_count < ST_SUBS) s_sub_count++;

    // Momentary / short-term
    float pm = recent_power(MOM_SUBS);
    float ps = recent_power(ST_SUBS);
    float lm = power_to_lufs(pm);
    float ls = power_to_lufs(ps);
    s_snap.lufs_m = lm;
    s_snap.lufs_s = ls;

    // Histograms (absolute gate)
    if (lm > ABS_GATE_LUFS) {
        int b = lufs_bin(lm);
        if (b >= 0) { s_hist_i_cnt[b]++; s_hist_i_pow[b] += pm; }
    }
    if (s_sub_count >= ST_SUBS && ls > ABS_GATE_LUFS) {
        int b = lufs_bin(ls);
        if (b >= 0) { s_hist_s_cnt[b]++; s_hist_s_pow[b] += ps; }
    }

    s_snap.lufs_i = power_to_lufs(gated_power(s_hist_i_cnt, s_hist_i_pow, -10.0f));
    s_snap.lra    = compute_lra();
    s_snap.tp_db  = (s_tp_max > 1e-7f) ? 20.0f * log10f(s_tp_max) : SILENCE_LUFS;
}

void loudness_process(const float *interleaved, uint32_t frames) {
    if (s_reset_req != s_reset_done) {
        s_reset_done = s_reset_req;
        apply_reset();
    }

    for (uint32_t i = 0; i < frames; i++) {
        float l = interleaved[i * 2 + 0];
        float r = interleaved[i * 2 + 1];

        // True peak on the raw signal
        s_tp_max = tp_push(0, l, s_tp_max);
        s_tp_max = tp_push(1, r, s_tp_max);

        // K-weighted mean square
        float kl = bq_run(&s_hp[0], bq_run(&s_shelf[0], l));
        float kr = bq_run(&s_hp[1], bq_run(&s_shelf[1], r));
        s_sub_acc += kl * kl + kr * kr;

        if (++s_sub_n >= SUB_BLOCK_FRAMES) {
            finish_sub_block();
        }
    }
}

void loudness_get(loudness_snapshot_t *out) {
    *out = s_snap;
}
