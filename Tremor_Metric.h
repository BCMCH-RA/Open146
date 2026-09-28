#pragma once

#include <stdint.h>
#include <stdbool.h>

// ─── Tremor severity levels (elderly display) ────────────────────
typedef enum {
    TREMOR_CALM = 0,
    TREMOR_MILD,
    TREMOR_MODERATE,
    TREMOR_SEVERE
} TremorLevel_t;

// ─── DSP config ──────────────────────────────────────────────────
// Causal pipeline (tremor DSP ported from the W146 sketch), no windowing and
// no lag: per axis, two cascaded RBJ band-pass stages -> magnitude squared
// across the three axes -> EMA on that mean-square -> sqrt() = severity.
// Every 200 Hz sample yields a fresh severity, so the on-screen band tracks the
// signal with a ~0.5 s time constant instead of freezing between 1 s window
// boundaries. The whole pipeline is ~30 flops/sample, negligible next to the
// I2C gyro read in the same task.
#define TREMOR_SAMPLE_RATE_HZ     200.0f   // imuTask runs at 200 Hz

// Classic essential-tremor band (4-12 Hz). The centre is the geometric mean and
// the edges are the CASCADE's half-power points, so both are measured off these
// two defines at init and retuning them here is enough to retune the filter.
// Stage count is fixed at 2 by Cascade_t in the .cpp.
#define TREMOR_BAND_LO_HZ         4.0f
#define TREMOR_BAND_HI_HZ         12.0f

// Each stage's Q and the gain normalisation are solved numerically at init
// rather than baked in as constants: a cascade's -3dB bandwidth is much
// narrower than a single stage's, and the raw cascade is not unity at f0.
// Skipping either correction silently rescales the reported severity.
#define TREMOR_EMA_ALPHA          0.01f    // tau ~= (1/fs)/alpha ~= 0.5 s

// ─── Severity bands ───────────────────────────────────────────────
// Units are dps of band-limited (4-12 Hz) gyro magnitude, i.e. a
// mean-square, so a steady single-axis tremor of PEAK amplitude A reads
// about 0.707*A. Keep that factor in mind when comparing against datasheets
// or published figures, which usually quote peak or peak-to-peak.
//
// CALIBRATION NOTE — these edges were originally 5 / 12 / 25 dps, which are
// unreachable for an actual tremor: they need 5.6 dps peak just to leave CALM
// and 28 dps for SEVERE, while real severe essential tremor is 10-20 dps peak.
// The display therefore sat on CALM and the band never tracked the patient.
// Measured severity for the current DSP (8 Hz tremor, before this change):
//
//   at rest .................. 0.00 dps   -> CALM
//   normal physiological ..... 0.71 dps   -> CALM
//   mild essential tremor .... 2.67 dps   -> was CALM     (under-reported)
//   moderate essential ....... 5.33 dps   -> was MILD
//   severe essential ........ 10.66 dps   -> was MILD     (under-reported)
//
// The edges below put each of those in the intended band. They are a
// provisional starting point derived from the table above, NOT a clinically
// validated scale — validate against real subjects before relying on it, and
// re-derive from measured readings on your actual population if needed.
//
// The CALM/MILD edge sits at 1.5 rather than 1.0 deliberately: normal
// physiological tremor reaches ~2 dps peak (severity ~1.4), so a 1.0 edge
// would put healthy readings into MILD. At 1.5, mild ET (severity 2.67) still
// clears it with room to spare.
#define TREMOR_CALM_MAX_DPS       1.5f     //  sev <  1.5 dps → CALM
#define TREMOR_MILD_MAX_DPS       3.0f     //  sev <  3.0 dps → MILD
#define TREMOR_MODERATE_MAX_DPS   8.0f     //  sev <  8.0 dps → MODERATE
                                           //  sev ≥  8.0 dps → SEVERE

// Margin the severity must clear to leave the band it is currently in, so a
// subject sitting near an edge settles on one reading instead of hunting
// between two. Without it the label flickers, which reads as a fault.
#define TREMOR_BAND_HYSTERESIS    0.10f    // 10%

void Tremor_Metric_Init(void);

// Feed one gyro sample in dps per axis — call at 200 Hz.
void Tremor_Metric_Feed(float gx_dps, float gy_dps, float gz_dps);

// Non-blocking: true when at least one new IMU sample has been processed since
// the previous call, giving the current level + severity. Safe to poll at any
// rate — it never blocks and never returns a stale-by-a-window value.
bool Tremor_Metric_Get(TremorLevel_t *out_level, float *out_severity_dps);

// Display helpers (high contrast for elderly)
const char *Tremor_Level_Text(TremorLevel_t level);    // "CALM"/"MILD"/"MODERATE"/"SEVERE"
uint32_t    Tremor_Level_Color(TremorLevel_t level);   // 0xRRGGBB
