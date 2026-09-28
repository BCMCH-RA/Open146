#include "Tremor_Metric.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Transposed Direct Form II: two state words instead of DF1's four, which
// matters for six filters running at 200 Hz on Core 1 next to the I2C read.
typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1, z2;
} Biquad_t;

typedef struct {
    Biquad_t stage1, stage2;
} Cascade_t;

static Cascade_t s_cX, s_cY, s_cZ;

// EMA state. Touched only by the IMU task (Core 1), so no volatility needed.
static float s_severityMS = 0.0f;

// Published for the display task (Core 0). Each is naturally aligned and 32
// bits or narrower, so a plain volatile read is atomic and cannot tear — this
// is what the old window-flag scheme was avoiding.
static volatile float    s_severity  = 0.0f;
static volatile uint8_t  s_level     = TREMOR_CALM;
static volatile uint32_t s_sampleSeq = 0;

// Only displayTask calls Tremor_Metric_Get(), so this needs no protection.
static uint32_t s_lastGetSeq = 0;


static void Biquad_Design(Biquad_t *b, float al, float cs) {
    const float a0 = 1.0f + al;
    b->b0 =  al / a0;              // gain normalised by the caller
    b->b1 =  0.0f;
    b->b2 = -al / a0;
    b->a1 = (-2.0f * cs) / a0;
    b->a2 = (1.0f - al) / a0;
    b->z1 = 0.0f;
    b->z2 = 0.0f;
}


// Analytic |H| of ONE stage at (cos w, sin w, cos 2w, sin 2w). Kept separate
// from the streaming step so the filter can be sized from its own maths rather
// than measured through a signal.
static float Biquad_Mag(const Biquad_t *b, float c1, float s1, float c2, float s2) {
    const float nr = b->b0 + b->b2 * c2;          // b1 == 0 for a band-pass
    const float ni = -b->b2 * s2;
    const float dr = 1.0f + b->a1 * c1 + b->a2 * c2;
    const float di = -b->a1 * s1 - b->a2 * s2;
    return sqrtf((nr * nr + ni * ni) / (dr * dr + di * di));
}


static inline float Biquad_Step(Biquad_t *b, float x) {
    const float y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x + b->z2 - b->a1 * y;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}


static inline float Cascade_Step(Cascade_t *c, float x) {
    return Biquad_Step(&c->stage2, Biquad_Step(&c->stage1, x));
}


// Size the filters once. Two things have to be got right for the severity
// number to mean anything in deg/s:
//
//  1. Bandwidth. Two cascaded 2nd-order band-pass stages narrow the half-power
//     bandwidth far more than the usual sqrt(2) rule of thumb suggests, so the
//     per-stage Q is bisected numerically to place the CASCADE's -3dB points on
//     the declared band edges. Doing it numerically also keeps TREMOR_BAND_*
//     authoritative if the band is ever retuned.
//  2. Gain. A cascade is not unity at f0 by construction, so b0 is scaled by
//     the measured in-band peak. Without this the reported severity is inflated
//     by whatever the raw coefficient ratio happens to be.
static void Tremor_FiltersInit(void) {
    const float f0 = sqrtf(TREMOR_BAND_LO_HZ * TREMOR_BAND_HI_HZ);
    const float w0 = 2.0f * (float)M_PI * f0 / TREMOR_SAMPLE_RATE_HZ;
    const float sw0 = sinf(w0), cw0 = cosf(w0);
    const float sw02 = sinf(2.0f * w0), cw02 = cosf(2.0f * w0);

    const float wl = 2.0f * (float)M_PI * TREMOR_BAND_LO_HZ / TREMOR_SAMPLE_RATE_HZ;
    const float swl = sinf(wl), cwl = cosf(wl);
    const float swl2 = sinf(2.0f * wl), cwl2 = cosf(2.0f * wl);

    const float wh = 2.0f * (float)M_PI * TREMOR_BAND_HI_HZ / TREMOR_SAMPLE_RATE_HZ;
    const float swh = sinf(wh), cwh = cosf(wh);
    const float swh2 = sinf(2.0f * wh), cwh2 = cosf(2.0f * wh);

    // The band-pass is symmetric about f0 in log-frequency, and 4..12 Hz is
    // symmetric about its geometric mean, so both edges are evaluated and
    // averaged: a single edge would just bias the band low or high.
    Biquad_t s;
    float qlo = 0.10f, qhi = 50.0f;
    for (int i = 0; i < 48; i++) {
        const float q = 0.5f * (qlo + qhi);
        Biquad_Design(&s, sw0 / (2.0f * q), cw0);
        const float peak = Biquad_Mag(&s, cw0, sw0, cw02, sw02);
        const float rlo = Biquad_Mag(&s, cwl, swl, cwl2, swl2) / peak;
        const float rhi = Biquad_Mag(&s, cwh, swh, cwh2, swh2) / peak;
        // r is ONE stage's gain ratio; TREMOR_STAGES of them cascade, so the
        // ratio this filter actually delivers is r*r. Halving it is what
        // targets 1/sqrt(2) on the cascade rather than on a single stage.
        if (0.5f * (rlo * rlo + rhi * rhi) > 0.70710678f) qlo = q;  // too wide
        else                                                    qhi = q;
    }

    const float q = 0.5f * (qlo + qhi);
    Biquad_Design(&s, sw0 / (2.0f * q), cw0);
    const float peak = Biquad_Mag(&s, cw0, sw0, cw02, sw02);
    s.b0 /= peak;                               // cascade is now unity at f0
    s.b2 /= peak;

    s_cX.stage1 = s; s_cX.stage2 = s;
    s_cY.stage1 = s; s_cY.stage2 = s;
    s_cZ.stage1 = s; s_cZ.stage2 = s;
}


void Tremor_Metric_Init(void) {
    Tremor_FiltersInit();
    s_severityMS = 0.0f;
    s_severity   = 0.0f;
    s_level      = TREMOR_CALM;
    s_sampleSeq  = 0;
    s_lastGetSeq = 0;
}


// Upper edge of each band: escalating past band N needs sev above edge[N].
// Indexed by the band being left, so edge[] is the ladder between levels.
static const float s_edge[3] = {
    TREMOR_CALM_MAX_DPS, TREMOR_MILD_MAX_DPS, TREMOR_MODERATE_MAX_DPS
};

// Move at most one band per sample, and only across an edge the severity has
// cleared with the hysteresis margin applied in the direction of travel.
static TremorLevel_t Band_Decide(TremorLevel_t cur, float sev) {
    for (int step = -1; step <= 1; step += 2) {
        const int idx = (int)cur + step;
        if (idx < 0 || idx > 3) continue;
        if (step > 0) {
            // Reaching the top band still has to clear the last edge; without
            // this, MODERATE would promote straight to SEVERE.
            if (sev >= s_edge[idx - 1] * (1.0f + TREMOR_BAND_HYSTERESIS))
                return (TremorLevel_t)idx;
        } else {
            if (sev < s_edge[idx] * (1.0f - TREMOR_BAND_HYSTERESIS))
                return (TremorLevel_t)idx;
        }
    }
    return cur;
}


void Tremor_Metric_Feed(float gx_dps, float gy_dps, float gz_dps) {
    const float fx = Cascade_Step(&s_cX, gx_dps);
    const float fy = Cascade_Step(&s_cY, gy_dps);
    const float fz = Cascade_Step(&s_cZ, gz_dps);

    const float magSq = fx * fx + fy * fy + fz * fz;
    s_severityMS = TREMOR_EMA_ALPHA * magSq + (1.0f - TREMOR_EMA_ALPHA) * s_severityMS;

    const float sev = sqrtf(s_severityMS);

    s_severity  = sev;
    s_level     = (uint8_t)Band_Decide((TremorLevel_t)s_level, sev);
    s_sampleSeq++;
}


bool Tremor_Metric_Get(TremorLevel_t *out_level, float *out_severity_dps) {
    const uint32_t seq = s_sampleSeq;
    if (seq == s_lastGetSeq) return false;   // IMU task has not run since last poll
    s_lastGetSeq = seq;
    if (out_level)        *out_level = (TremorLevel_t)s_level;
    if (out_severity_dps) *out_severity_dps = s_severity;
    return true;
}


const char *Tremor_Level_Text(TremorLevel_t level) {
    switch (level) {
        case TREMOR_CALM:     return "CALM";
        case TREMOR_MILD:     return "MILD";
        case TREMOR_MODERATE: return "MODERATE";
        case TREMOR_SEVERE:   return "SEVERE";
        default:              return "CALM";
    }
}


uint32_t Tremor_Level_Color(TremorLevel_t level) {
    switch (level) {
        case TREMOR_CALM:     return 0x00E676;
        case TREMOR_MILD:     return 0xFF5252;
        case TREMOR_MODERATE: return 0xFFA726;
        case TREMOR_SEVERE:   return 0xF44336;
        default:              return 0x00E676;
    }
}
