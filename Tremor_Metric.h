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

// Skirt steepness. Each 2nd-order stage contributes 6 dB/octave beyond the
// band edge, so the stage count sets how fast non-tremor motion is rejected.
//
// This matters more than the -3 dB points, which the bisection in the .cpp
// pins to TREMOR_BAND_* exactly. A wrist-worn device is exposed to large
// deliberate swings, and those are slow: a 2nd-order-per-axis pair (4th order,
// 12 dB/octave) let a 20 dps 2 Hz arm swing through at -12 dB, i.e. 3.4 dps
// severity, which reads as MODERATE. Order 3 (6th order, 18 dB/octave) pushes
// the same swing below the CALM/MILD edge while leaving the in-band amplitude
// and the 4..12 Hz edges untouched.
#define TREMOR_STAGES             3

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

// ─── Level confirmation (dwell) ────────────────────────────────────
// Hysteresis alone only stops the label hunting while the severity sits ON an
// edge. It does nothing about a tremor that repeatedly crosses one: Band_Decide
// runs every sample, so at 200 Hz the label would still change several times a
// second. On a wrist-worn display shown to a patient that strobing reads as a
// faulty device.
//
// So a different band must be SUSTAINED before it is displayed. The two
// directions deliberately differ:
//
//   escalate   0.5 s — a wearer must be told promptly that the tremor has
//                      worsened, so confirmation is kept short.
//   de-escalate 3.0 s — a tremor that briefly subsides (cough, changing grip,
//                      lowering the arm) must not drop the label immediately,
//                      or the display flips between two colours. Settling back
//                      down is allowed to be lazy.
//
// At 200 Hz that is 100 samples up and 600 down. Because the band-pass already
// removes everything outside 4..12 Hz, this is a dwell on TREMOR, not on
// general movement. Both values are seconds so they can be retuned without
// doing arithmetic.
#define TREMOR_DWELL_ESCALATE_S    0.5f
#define TREMOR_DWELL_DEESCALATE_S  3.0f

// ─── Movement rejection (narrowband ratio) ────────────────────────
// The band-pass alone is NOT enough to keep ordinary movement out of the
// bands, and this is a property of any fixed band, not a tuning mistake. Near a
// band edge a steep skirt is still shallow, and deliberate movement at the
// wrist is 5-30x larger in amplitude than a tremor, so enough leaks through to
// cross an absolute dps threshold. Measured on the 6th-order filter:
//
//   mild / moderate / severe tremor (8 Hz) ...... 0.98 / 1.00 / 1.00
//   lifting a cup (1 Hz, 60 dps) ................ 0.03
//   brisk arm swing (1.2 Hz, 120 dps) ........... 0.07
//   walking arm swing (2 Hz, 50 dps) ............ 0.20
//   pouring a drink (2.5 Hz, 60 dps) ............ 0.32
//   hand rattle (13 Hz, 25 dps) ................. 0.63
//
// Tremor is narrowband and movement is broadband, so the discriminator is how
// much of the total signal survives the band-pass:
//
//   ratio = rms(in-band 4-12 Hz) / rms(total gyro magnitude)
//
// For a pure tone this is exactly the cascade's |H| at that frequency, so it
// approaches 1.0 inside the band and falls away on both skirts. Requiring it to
// clear a floor before the label may RISE separates the two populations by
// roughly an order of magnitude, which no amount of filter order achieves.
//
// Gating escalation only, on purpose:
//   - Under-reporting a worsening tremor is the cheap error; showing a false
//     escalation because someone reached for a cup is the expensive one.
//   - Relaxation is left ungated, so a transient movement cannot drag the label
//     down. It still falls on its own once the EMA decays and the 3 s dwell
//     elapses.
//
// A real subject's tremor is never a pure tone, so a genuine 8 Hz tremor sits
// slightly below 1.0 and a floor that is too high would suppress real readings.
// 0.50 is a starting point chosen to clear the movement cases above with margin;
// treat it as UNVALIDATED and re-derive it from recorded walks before trusting
// it clinically.
#define TREMOR_NARROWBAND_MIN      0.50f

void Tremor_Metric_Init(void);

// Feed one gyro sample in dps per axis — call at 200 Hz.
void Tremor_Metric_Feed(float gx_dps, float gy_dps, float gz_dps);

// Non-blocking: true when at least one new IMU sample has been processed since
// the previous call, giving the current level + severity. Safe to poll at any
// rate — it never blocks and never returns a stale-by-a-window value.
bool Tremor_Metric_Get(TremorLevel_t *out_level, float *out_severity_dps);

// Narrowband ratio, i.e. how much of the total gyro magnitude survives the
// 4-12 Hz band-pass. Same "no new sample" rule as Tremor_Metric_Get: returns
// false and writes nothing when the IMU task has not run since the last call.
// A UI can use it to show a "measuring" hint while a value below
// TREMOR_NARROWBAND_MIN is suppressing escalation, instead of leaving the
// patient looking at a CALM label next to a nonzero severity number.
bool Tremor_Metric_GetNarrowband(float *out_ratio);

// Display helpers (high contrast for elderly)
const char *Tremor_Level_Text(TremorLevel_t level);    // "CALM"/"MILD"/"MODERATE"/"SEVERE"
uint32_t    Tremor_Level_Color(TremorLevel_t level);   // 0xRRGGBB
