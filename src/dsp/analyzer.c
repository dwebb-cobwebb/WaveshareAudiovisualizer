#include "dsp/analyzer.h"
#include "dsp/loudness.h"
#include "app.h"

#include <math.h>
#include <string.h>

#include "arm_math.h"     // CMSIS-DSP

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define HOP             (AV_FFT_SIZE / 2)          // 50% overlap
#define ANALYSIS_FPS    ((float)AV_SAMPLE_RATE_HZ / (float)HOP)
#define FLOOR_DB        (-60.0f)
#define CLIP_THRESH     (0.997f)                   // ~ -0.03 dBFS
#define MAG_GAIN        (4.0f / (float)AV_FFT_SIZE) // window+rfft normalization

// Oscilloscope trigger search window and fixed post-trigger display span (see
// scope_find_trigger). The span is fixed rather than "whatever is left after
// the trigger" so the timebase (samples/column) can't drift frame to frame —
// a variable span pinned the trigger sample but let everything to its right
// stretch/squeeze differently every frame. Span <= search limit guarantees
// enough samples remain regardless of where the trigger lands.
#define SCOPE_TRIGGER_SEARCH   (AV_FFT_SIZE / 2)
#define SCOPE_SPAN             (AV_FFT_SIZE / 2)

// ---------------------------------------------------------------------------
// State (DSP core only)
// ---------------------------------------------------------------------------
static arm_rfft_fast_instance_f32 s_fft;
static float s_window[AV_FFT_SIZE];
static uint16_t s_band_lo[AV_NUM_BANDS];   // first FFT bin of each band (inclusive)
static uint16_t s_band_hi[AV_NUM_BANDS];   // last FFT bin of each band (inclusive)

static float s_band_smooth[AV_NUM_BANDS];
static float s_band_peak[AV_NUM_BANDS];
static int   s_band_peak_hold[AV_NUM_BANDS];

static int   s_clip_hold_l;
static int   s_clip_hold_r;
static uint32_t s_frame_id;

// Scratch (kept static to avoid large stack frames — core1's stack is a
// dedicated 4K SCRATCH bank, and VisualizerState is large enough with the
// oscilloscope trace arrays that leaving it and `mag` on the stack risked
// overflowing that bank).
static float s_in_mono[AV_FFT_SIZE];
static float s_fft_out[AV_FFT_SIZE];
static float s_stereo[AV_FFT_SIZE * AV_CHANNELS];
static VisualizerState s_vs_scratch;
static float s_mag[AV_FFT_BINS];

// ---------------------------------------------------------------------------
// Double buffer for VisualizerState (vis_state.h API)
// ---------------------------------------------------------------------------
static VisualizerState s_vs[2];
static volatile uint8_t s_vs_front;   // index the consumer should read

void vis_state_init(void) {
    memset(s_vs, 0, sizeof(s_vs));
    s_vs_front = 0;
}

void vis_publish(const VisualizerState *s) {
    uint8_t back = s_vs_front ^ 1u;
    s_vs[back] = *s;
    __asm volatile("" ::: "memory");
    s_vs_front = back;   // single-word atomic swap
}

void vis_acquire(VisualizerState *out) {
    uint8_t f = s_vs_front;
    __asm volatile("" ::: "memory");
    *out = s_vs[f];
}

uint32_t vis_frame_id(void) {
    return s_vs[s_vs_front].frame_id;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static void build_window(void) {
    for (int n = 0; n < AV_FFT_SIZE; n++) {
        s_window[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * n / (AV_FFT_SIZE - 1)));
    }
}

static void build_bands(void) {
    const float bin_hz = (float)AV_SAMPLE_RATE_HZ / (float)AV_FFT_SIZE;
    const float log_lo = log10f(AV_BAND_FREQ_LOW);
    const float log_hi = log10f(AV_BAND_FREQ_HIGH);
    int prev_hi = -1;
    for (int b = 0; b < AV_NUM_BANDS; b++) {
        float f0 = powf(10.0f, log_lo + (log_hi - log_lo) * (float)b / AV_NUM_BANDS);
        float f1 = powf(10.0f, log_lo + (log_hi - log_lo) * (float)(b + 1) / AV_NUM_BANDS);
        int lo = (int)floorf(f0 / bin_hz);
        int hi = (int)ceilf(f1 / bin_hz);
        if (lo < 1) lo = 1;                       // skip DC
        if (hi >= AV_FFT_BINS) hi = AV_FFT_BINS - 1;
        if (lo <= prev_hi) lo = prev_hi + 1;      // keep bands disjoint
        if (hi < lo) hi = lo;
        s_band_lo[b] = (uint16_t)lo;
        s_band_hi[b] = (uint16_t)hi;
        prev_hi = hi;
    }
}

void analyzer_init(void) {
    arm_rfft_fast_init_f32(&s_fft, AV_FFT_SIZE);
    build_window();
    build_bands();
    loudness_init();
    memset(s_band_smooth, 0, sizeof(s_band_smooth));
    memset(s_band_peak, 0, sizeof(s_band_peak));
    memset(s_band_peak_hold, 0, sizeof(s_band_peak_hold));
    s_clip_hold_l = s_clip_hold_r = 0;
    s_frame_id = 0;
}

// ---------------------------------------------------------------------------
// Per-frame processing
// ---------------------------------------------------------------------------
static float mag_to_norm(float mag) {
    float db = 20.0f * log10f(mag * MAG_GAIN + 1e-9f);
    float n = (db - FLOOR_DB) / (0.0f - FLOOR_DB);
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    return n;
}

static inline int8_t scope_quantize(float v) {
    float s = v * 127.0f;
    if (s > 127.0f) s = 127.0f;
    if (s < -127.0f) s = -127.0f;
    return (int8_t)s;
}

// Oscilloscope trigger: first rising zero-crossing of the L+R mono mix,
// searched over the first half of the window so at least half a window of
// data remains to display afterwards. Falls back to 0 (free-run) if the
// signal never crosses in that span — cheap and good enough for a musical
// scope; it isn't a lab instrument with deep pre-trigger memory.
static int scope_find_trigger(const float *stereo) {
    const int limit = SCOPE_TRIGGER_SEARCH;
    float prev = 0.5f * (stereo[0] + stereo[1]);
    for (int n = 1; n <= limit; n++) {
        float cur = 0.5f * (stereo[n * 2] + stereo[n * 2 + 1]);
        if (prev < 0.0f && cur >= 0.0f) return n;
        prev = cur;
    }
    return 0;
}

bool analyzer_process(AudioRing *ring) {
    if (ring_available(ring) < AV_FFT_SIZE) return false;

    ring_peek(ring, s_stereo, AV_FFT_SIZE);
    ring_advance(ring, HOP);

    // Loudness runs on the HOP frames being consumed this iteration, so with
    // 50% FFT overlap every sample is measured exactly once.
    loudness_process(s_stereo, HOP);

    // Time-domain stats + windowed mono mix.
    float sum_l2 = 0.f, sum_r2 = 0.f, sum_lr = 0.f;
    float peak_l = 0.f, peak_r = 0.f;

    // Oscilloscope: min/max envelope per display column, flushed as the
    // sample index crosses into the next column's span. Trigger discards
    // samples before the first rising zero-crossing so periodic/tonal
    // content holds still instead of sliding frame to frame; a FIXED
    // SCOPE_SPAN (not "whatever is left") is remapped across AV_SCOPE_COLS
    // columns so the timebase can't drift between frames.
    VisualizerState *vsp = &s_vs_scratch;
    int t0 = scope_find_trigger(s_stereo);
    int col = 0;
    float col_min_l = 1e9f, col_max_l = -1e9f;
    float col_min_r = 1e9f, col_max_r = -1e9f;

    for (int n = 0; n < AV_FFT_SIZE; n++) {
        float l = s_stereo[n * 2 + 0];
        float r = s_stereo[n * 2 + 1];
        sum_l2 += l * l;
        sum_r2 += r * r;
        sum_lr += l * r;
        float al = fabsf(l), ar = fabsf(r);
        if (al > peak_l) peak_l = al;
        if (ar > peak_r) peak_r = ar;
        s_in_mono[n] = 0.5f * (l + r) * s_window[n];

        int rel = n - t0;
        if (rel < 0 || rel >= SCOPE_SPAN) continue;
        int c = (rel * AV_SCOPE_COLS) / SCOPE_SPAN;
        if (c != col) {
            vsp->scope_min_l[col] = scope_quantize(col_min_l);
            vsp->scope_max_l[col] = scope_quantize(col_max_l);
            vsp->scope_min_r[col] = scope_quantize(col_min_r);
            vsp->scope_max_r[col] = scope_quantize(col_max_r);
            col = c;
            col_min_l = col_max_l = l;
            col_min_r = col_max_r = r;
        } else {
            if (l < col_min_l) col_min_l = l;
            if (l > col_max_l) col_max_l = l;
            if (r < col_min_r) col_min_r = r;
            if (r > col_max_r) col_max_r = r;
        }
    }
    vsp->scope_min_l[col] = scope_quantize(col_min_l);
    vsp->scope_max_l[col] = scope_quantize(col_max_l);
    vsp->scope_min_r[col] = scope_quantize(col_min_r);
    vsp->scope_max_r[col] = scope_quantize(col_max_r);

    // Goniometer: raw (untriggered) sample pairs spread evenly across the
    // whole window — a stereo scatter plot wants the full spatial
    // distribution, not a phase-aligned slice, so no trigger is applied here.
    // STEP 2: written every frame now, but nothing reads gonio_l/gonio_r yet.
    for (int i = 0; i < AV_GONIO_POINTS; i++) {
        int n = (i * AV_FFT_SIZE) / AV_GONIO_POINTS;
        vsp->gonio_l[i] = scope_quantize(s_stereo[n * 2 + 0]);
        vsp->gonio_r[i] = scope_quantize(s_stereo[n * 2 + 1]);
    }

    // FFT -> magnitude -> bands.
    arm_rfft_fast_f32(&s_fft, s_in_mono, s_fft_out, 0);
    s_mag[0] = fabsf(s_fft_out[0]);                 // DC (bin 0 stored in out[0])
    for (int k = 1; k < AV_FFT_BINS; k++) {
        float re = s_fft_out[2 * k];
        float im = s_fft_out[2 * k + 1];
        s_mag[k] = sqrtf(re * re + im * im);
    }

    const int hold_frames = (int)(ANALYSIS_FPS * (AV_PEAK_HOLD_MS / 1000.0f));

    for (int b = 0; b < AV_NUM_BANDS; b++) {
        float acc = 0.f;
        int cnt = 0;
        for (int k = s_band_lo[b]; k <= s_band_hi[b]; k++) { acc += s_mag[k]; cnt++; }
        float band = mag_to_norm(cnt > 0 ? acc / cnt : 0.f);

        // Attack/decay ballistics.
        float prev = s_band_smooth[b];
        float coeff = (band > prev) ? AV_BAND_ATTACK : AV_BAND_DECAY;
        prev += (band - prev) * coeff;
        s_band_smooth[b] = prev;
        vsp->bands[b] = prev;

        // Per-band peak hold.
        if (prev >= s_band_peak[b]) {
            s_band_peak[b] = prev;
            s_band_peak_hold[b] = hold_frames;
        } else if (s_band_peak_hold[b] > 0) {
            s_band_peak_hold[b]--;
        } else {
            s_band_peak[b] *= (1.0f - AV_BAND_DECAY);
        }
        vsp->band_peak[b] = s_band_peak[b];
    }

    vsp->rms_l = sqrtf(sum_l2 / AV_FFT_SIZE);
    vsp->rms_r = sqrtf(sum_r2 / AV_FFT_SIZE);
    vsp->peak_l = peak_l;
    vsp->peak_r = peak_r;

    float denom = sqrtf(sum_l2 * sum_r2);
    vsp->correlation = (denom > 1e-9f) ? (sum_lr / denom) : 0.0f;

    loudness_snapshot_t lu;
    loudness_get(&lu);
    vsp->lufs_m = lu.lufs_m;
    vsp->lufs_s = lu.lufs_s;
    vsp->lufs_i = lu.lufs_i;
    vsp->lra    = lu.lra;
    vsp->tp_db  = lu.tp_db;

    if (peak_l >= CLIP_THRESH) s_clip_hold_l = hold_frames;
    else if (s_clip_hold_l > 0) s_clip_hold_l--;
    if (peak_r >= CLIP_THRESH) s_clip_hold_r = hold_frames;
    else if (s_clip_hold_r > 0) s_clip_hold_r--;
    vsp->clip_l = (s_clip_hold_l > 0);
    vsp->clip_r = (s_clip_hold_r > 0);

    vsp->frame_id = ++s_frame_id;
    vis_publish(vsp);
    return true;
}

void analyzer_core1_main(void) {
    analyzer_init();
    for (;;) {
        if (!analyzer_process(&g_audio_ring)) {
            // Not enough samples yet; brief spin. (Could use __wfe/sev pairing.)
            for (volatile int i = 0; i < 256; i++) { __asm volatile("nop"); }
        }
    }
}
