# BCMCH Tremor Monitor v5.1.9 — CSV download protocol redesign (read-based, not notify-based)

## Why this round is different

Every fix through v5.1.8 targeted a specific plausible cause of the
200-byte truncation (battery, deployment staleness, live-preview
contention, connection interval) and each one was a real, worthwhile fix
on its own merits -- but the core symptom kept recurring even after all
of them were confirmed active simultaneously on a healthy battery and a
freshly-deployed page. At that point, continuing to tune notify-based
BLE parameters stopped being the right use of effort. BLE notifications
are fundamentally unacknowledged at the application level -- the
firmware has no way to know whether a given `notify()` actually arrived,
and Android's handling of sustained notification bursts has proven
fragile in this environment in ways that resisted every angle of tuning.

## The redesign (P5)

CSV downloads no longer use BLE notifications at all. Instead:

1. `XFER_CTRL` write (unchanged) starts a pull and reports the total size
   via `XFER_INFO`, same as before.
2. The browser now explicitly requests each chunk by writing a 4-byte
   offset to a new characteristic, `XFER_SEEK`.
3. It then does a confirmed `readValue()` on `XFER_DATA` to fetch exactly
   that chunk.
4. If a chunk's write or read fails (or times out after 4s), it's
   retried up to 3 times before the whole download aborts with a clear
   message -- partial data is never silently presented as a complete
   file.

This is a fundamentally different reliability model: every chunk is a
discrete, confirmed request/response the browser controls the pacing
of, rather than a stream of unacknowledged pushes the firmware fires off
and hopes arrive. There's no longer a way for a chunk to simply vanish
without either side knowing.

Chunk size is also larger now (400 bytes vs. the old 200) since reads
aren't bounded by the same practical constraints as notifications --
Web Bluetooth's `readValue()` transparently handles the GATT "read
blob" continuation for values larger than one ATT_MTU packet, so this
didn't need manual fragmentation logic on top of the chunking that
already existed.

## What's now dead code (kept, not deleted, for diff clarity)

`bleTransferPump()` (the old notify-push loop) is still defined in the
firmware but no longer called from `loop()`. `XFER_DATA`'s BLE property
changed from NOTIFY to READ, so the old push mechanism wouldn't work
against it even if it were still wired in.

## What to expect

Downloads should feel a bit different: the progress bar will advance in
visibly discrete steps (one per confirmed chunk) rather than a smooth
stream, and you may see the occasional "Chunk at offset X failed,
retrying" log line under imperfect BLE conditions -- that's the new
resilience working as intended, not a new problem. A `NOTE: received
byte count doesn't match...` message would mean something is still
generically wrong, but a full download either completing cleanly or
failing with an explicit, retryable error (rather than silently handing
you a 200-byte fragment) is the actual goal of this change.

Please test again -- this is the one I'm most confident actually
addresses the root cause, since it removes the specific mechanism
(unacknowledged notifications) every prior theory pointed back to one
way or another.

---

# BCMCH Tremor Monitor v5.1.8 — deployment confirmed current; softened connection interval

## Good news: the deployment question is finally settled

Your last screenshot's log text ("Battery is low -- the device will
refuse this download rather than risk a failed transfer") matches the
v5.1.4 wording exactly. That confirms, for certain, that `index.html` on
`mch-ra.github.io` is current -- the recurring 200-byte pattern in
earlier rounds was not explained by a stale page. It's also good
confirmation that the battery guard (P1) and the corrected calibration
(P2) are working together correctly: 3202mV is being read accurately and
the guard is refusing the download rather than risking a bad transfer,
exactly as designed.

## Softened the connection interval request (P4b)

A `GATT Error Unknown` + disconnect appeared in the round right after
the original connection-interval request (P4, 7.5-15ms -- the BLE
spec's absolute minimum) went out. That range is known to be poorly
supported on some Android/chipset combinations. Widened to 15-30ms --
still a real improvement over letting Android pick its own conservative
default, but a meaningfully safer range less likely to destabilize the
connection itself. Serial log text updated to match.

## Where this leaves us

With deployment confirmed current, calibration confirmed accurate, and
the interval request now more conservative, the next attempt (once
charged past 3400mV) will be the first real test of the P3 (preview
contention removed) + P4b (softened interval) combination together. If
it's still not clean, the Serial log from that attempt -- especially
whether `[BLE] Requested fast connection interval` appears and what
`accepted-for-send` shows -- will tell us whether to keep tuning BLE
parameters or look elsewhere entirely.

---

# BCMCH Tremor Monitor v5.1.7 — request a fast BLE connection interval

## What "10 minutes, no stall, no completion" told us

This was a different, more useful symptom than any before: your progress
bar showed a small amount of movement, then nothing for the full 10
minutes — no stall message (which needs 6s of *zero* data to trigger),
no completion. That means data was trickling in just often enough to
keep resetting the stall timer, but nowhere near fast enough to finish.
That's consistent with the P3 contention fix having worked (no more
competing preview stream), but exposing a *second*, independent
bottleneck underneath it: the actual negotiated BLE connection interval.

## The bug: software pacing assumed a fast connection interval that was never requested

`BLE_XFER_SEND_MS=15` assumes the phone and device can exchange a
notification roughly every 15ms. But without the peripheral explicitly
requesting a short connection interval, Android commonly negotiates a
much more conservative one by default -- and if only one notification
fits per connection event, actual throughput is capped by that interval,
not by the firmware's 15ms software delay. A 100-200ms real interval
would produce exactly the "slow trickle, never finishes" pattern you saw
-- a few hundred bytes/second instead of the ~13KB/s the pacing was
designed around.

## Fix (P4)

The firmware now explicitly requests a fast connection interval
(7.5-15ms, the BLE-spec minimum range) right after connecting, via the
NimBLE-Arduino `requestConnParams()` API (verified against the actual
arduino-esp32 3.x source for this exact core version). The request is
deferred by 1.5 seconds after connection rather than sent immediately --
Android is known to reject a peripheral-initiated connection parameter
change requested too soon after connecting, before the link has settled.

Serial will now print `[BLE] Requested fast connection interval
(7.5-15ms), accepted-for-send=1` shortly after each connection -- useful
to confirm the request itself went out even if the phone's Bluetooth
stack doesn't ultimately honor the exact numbers (the OS makes the final
call; this is a request, not a guarantee).

## What to expect

Downloads should be noticeably faster and more reliable now that both
the contention issue (P3) and the throughput ceiling (P4) are addressed.
Please test again with the Serial log open if possible -- the new `[BLE]
Requested fast connection interval` line is worth including so we can
confirm the request path is working even if throughput still isn't
where it should be.

---

# BCMCH Tremor Monitor v5.1.6 — fixed: live preview stream competing with downloads

## What your latest test proved

This round ruled out everything else conclusively: the log text matched
the current build exactly (confirming `index.html` is genuinely
deployed), and the battery read correctly (3770mV, no LOW flag —
confirming the v5.1.5 calibration fix). Yet the download still stalled
completely (0 of 346,651 bytes). That means it's a real bug, independent
of deployment staleness or power — good, because that's finally
something I can actually fix with confidence rather than guess at.

## The bug: two notification streams sharing one channel

The live tremor-severity preview (the numbers that update in the app
while you're just watching, not downloading) streams over BLE
continuously, ~20 times a second, **regardless of whether you're
recording or downloading**. It shares the exact same notification-pacing
gate as the CSV transfer's chunk-sending code, and it runs *first* in
the firmware's main loop, every single iteration — so for the entire
duration of a 30-40+ second download, these two streams were
continuously contending for the same BLE notification channel and
connection bandwidth. This is a well-known class of problem on ESP32's
BLE stack: two independent notification producers on one connection can
stall each other, especially under sustained load.

This fits everything we've seen across every failed attempt so far —
varying amounts of data getting through (sometimes one chunk, sometimes
none, once the full thing), regardless of battery or deployment version
— because it was never about power or code freshness, it was contention
that could win or lose unpredictably depending on exact timing.

## Fix (P3)

The live preview stream now pauses completely while a CSV pull is
active — in two places: it stops being generated inside the sampling
task, and any frame that was already queued right as a transfer started
also gets skipped rather than stealing a notification slot from the
download. It resumes automatically the instant the transfer finishes.
The download now has the entire notification channel to itself.

## What to expect

The live "Current/Low/High" numbers on the page will simply stop
updating for the duration of a download (expected — that stream is
paused) and resume immediately once it completes. Please test again and
let me know how it goes, ideally with the Serial log again if it's still
not clean — but I'm fairly confident this was the actual bug.

---

# BCMCH Tremor Monitor v5.1.5 — battery reading was wrong; root cause still open

## Correction to my earlier theory

Your multimeter reading (3.8V, genuinely healthy) disproves the battery
theory from v5.1.4 — the real battery was never actually low. I want to
be direct about that rather than let it stand: I was wrong that marginal
battery power was the root cause of the transfer failures.

## What was actually wrong: the voltage divider ratio

`batteryVoltageMV()` assumed a 2:1 voltage divider on this board
(`adcMv * 2`). Working back from your numbers: the code was reporting
2682mV, implying it measured ~1341mV at the ADC pin -- but your real
battery is 3800mV, giving an actual ratio of **2.834:1**, not 2:1. Fixed
(P2) using that measured ratio as a named constant
(`BATTERY_DIVIDER_RATIO`), with recalibration instructions in the code
comment if a different unit/board revision needs adjusting further. This
was a real, independently-worth-fixing bug: a permanently-wrong-low
reading would have caused the v5.1.4 download guard to block CSV
downloads forever on a perfectly healthy battery, regardless of the
original transfer bug.

**This is a one-point calibration** (derived from a single multimeter
comparison), so treat it as a good first correction rather than a
perfectly precise one -- if the reported voltage still looks off after
flashing, compare against a multimeter once more and adjust
`BATTERY_DIVIDER_RATIO` proportionally.

## Root cause of the actual transfer bug: back to open

With battery ruled out, the 200-byte truncation (header + 2 rows + a
partial 3rd-row timestamp) is unexplained again. The fact that it's now
reproducing **identically across multiple tries** (rather than varying,
as it did in earlier RF/power-flakiness-consistent attempts) points at a
deterministic logic bug rather than a physical/RF issue -- which is
actually a more tractable thing to chase than intermittent flakiness.

**Before going further, please confirm the deployed `index.html` is
actually current.** The exact symptom you're seeing now matches the very
first failure, before any of the transfer-logic fixes since. Check by
viewing the live page's source at `mch-ra.github.io` and searching for
the string `"Screen wake lock"` or `"battery is low"` -- if neither
appears, the site is still serving an old build, and redeploying +
hard-refreshing should be the next step before we look for a new bug in
logic that may already be fixed.

---

# BCMCH Tremor Monitor v5.1.4 — battery guard for CSV downloads

## What your Serial log + screenshot proved

Two attempts, side by side:
- **222,702 bytes**: `CSV pull started` → ... → `CSV pull complete`.
  Fully succeeded, device-side.
- **234,974 bytes** (a later attempt, battery had dropped further to
  2682mV, phone at 11%): **0 bytes received**, stalled completely.

Same mechanism, same code — the only thing that changed between them was
the power situation getting worse. That's much stronger evidence than
anything I had before, and it points squarely at battery/power rather
than the phone's screen or backgrounding.

## Fix: refuse the download outright on low battery (P1)

Rather than let a marginal battery produce an unpredictable partial/failed
transfer, the firmware now checks `batLow` (already tracked, threshold
3400mV) before starting a CSV pull. If low, it **refuses immediately**
with a clear Serial message (`CSV pull REFUSED -- battery low (X mV)`)
and sends a distinct sentinel value back to the browser, which shows
"Device refused the download: battery is low" instead of attempting a
transfer that's likely to fail anyway.

The web page also now checks the last known battery status **before**
even trying — so you see the warning immediately on tapping Download,
without waiting for a round trip to the device.

## About "connected via USB power"

Worth flagging: your battery still read 2682mV (LOW) in that screenshot
even with USB connected. If that USB connection was to the **device**
itself (not just your phone), and the reading didn't recover after a few
minutes of charging, that's worth treating as a signal about the
battery's health, not just "needs a top-up" — 2682mV is quite low for a
single-cell LiPo (typical safe low-voltage cutoffs sit around
3.0–3.3V), and depending on this board's power-path design, the system
may still be drawing from the battery rail even while USB is present
rather than running directly off USB power. Worth watching whether the
voltage climbs properly with a full charge cycle, or a battery swap may
be worth considering if it doesn't.

## Recommended next test

1. Charge the device fully (give it real time on USB, not just a brief
   plug-in) before the next attempt.
2. Confirm the "Battery" line in the device-info panel shows a healthy
   voltage (comfortably above 3400mV) before hitting Download.
3. If you still have the CSV from the **successful** 222,702-byte
   transfer, it's worth a quick check that it's complete and well-formed
   — that would close the loop and confirm the whole pipeline works
   correctly end-to-end when power is adequate.

---

# BCMCH Tremor Monitor v5.1.3 — CSV transfer reliability fix

## What the screenshot proved

Your screenshot was decisive: the device genuinely had **8,775 rows /
538,625 bytes** buffered (confirmed via the STATUS panel), yet the
downloaded file was still only ~200 bytes. This ruled out "just a short
test recording" definitively — something real was going wrong in the
transfer itself.

Two data points, one from each of your download attempts:
- First attempt: file started exactly at the CSV header
- Second attempt: file started **mid-row**, matching neither the start
  nor a clean boundary

Both were **exactly 200 bytes** — exactly one `BLE_XFER_CHUNK_BYTES`
chunk. I traced through `imuTask`'s row-append code and confirmed it's
correctly mutex-protected end-to-end (a row is either fully written and
counted, or not at all — there's no code path that produces a "total"
landing mid-row on the firmware side). That, combined with two different
attempts producing two different arbitrary 200-byte fragments, points at
the **transfer itself failing partway through and only one fragment
surviving** — not a firmware data-integrity bug.

## Leading hypothesis (can't confirm without a Serial log, but it fits everything observed)

At 200 bytes every 15ms, downloading 538,625 bytes takes **roughly 40
seconds**. On Android, if the screen times out or the browser tab gets
backgrounded during that window, BLE notification delivery to the page's
JavaScript can be throttled or dropped by the OS — while the firmware
keeps sending regardless, since `notify()` has no application-level
acknowledgment and has no way to know anyone stopped listening. Whatever
fragment happens to still be "in flight" when things resume is what
survives. This fits both odd details: a ~40 second wait is a very
plausible amount of time for a screen to time out, and the *arbitrary*
starting point of the second file (not the header, not a clean boundary)
is exactly what you'd expect from "whichever notification happened to
survive," not a specific bug in a specific step.

**I want to be upfront**: this is my best explanation given everything
available, but I haven't been able to confirm it against a Serial log.
If it happens again, the single most useful thing you could grab is the
Serial monitor's `[BLE][XFER] CSV pull started, N bytes` line — if `N`
matches the real buffer size, that all but confirms this theory.

## Fixes (don't require confirming the exact cause)

**1. Screen Wake Lock during download.** The page now requests a screen
wake lock for the duration of a CSV download and releases it when done.
This directly prevents the screen-off half of the failure mode. It can't
prevent you switching away to another app mid-download — please stay on
this tab until the progress bar completes.

**2. Loud failure instead of silent wrong success.** This is the more
important fix: previously, if a transfer went wrong in this way, the
page had no way to notice — it just downloaded whatever it got and
called it done. Now, the page remembers the buffer size from its last
status check, and if the reported download size is suspiciously smaller
than that, it **aborts with a clear warning and asks you to retry**,
rather than quietly handing you a truncated file that looks legitimate.
This is the fix that actually matters for a medical-monitoring
tool — a clinician should never be able to mistake a truncated file for
a complete session.

## What to do if it's still slow or fails

- Keep the phone screen on and this browser tab in the foreground for
  the whole download — for a large buffer that's under a minute, but
  it's not instant.
- If you still hit the "far smaller than expected" warning, just try
  Download again — the buffer isn't cleared by a failed attempt (only
  Refresh clears it), so nothing is lost by retrying.
- If it keeps failing, the Serial monitor's `[BLE][XFER]` lines from a
  failed attempt would let us look at the firmware side of the same
  transfer and confirm or rule out the hypothesis above definitively.

---

# BCMCH Tremor Monitor v5.1.2 — CSV data-loss fix

## Root cause of "3000+ rows recorded, only 2 rows downloaded"

Confirmed in the code: `start_imu_rec` called `resetCSV()` internally
("gives this recording a clean BLE-downloadable CSV buffer" — original
design intent, present since v5.0.0, not something introduced by the
tremor changes). Every time **Start** was pressed — including a second
press after a Stop, which is a completely normal thing to do — the RAM
buffer that Download reads from was silently wiped back to just the
header. If Start got pressed again (for any reason — double-checking
recording was on, starting a new segment, etc.) before the previous
segment was downloaded, that data was gone. The "2 rows" you saw is
consistent with: buffer wiped, then ~1 sample accumulated before Stop
was pressed again.

**Fix (D1)**: `start_imu_rec` no longer clears the buffer. Multiple
Start/Stop segments across a session now **accumulate into one growing
downloadable file** — which is also just a better fit for how tremor
monitoring actually gets used (many start/stop segments through a wake
cycle, not one continuous take). The buffer is now cleared **only** by
an explicit "Refresh buffer" press. At 4MB (PSRAM), that's many hours of
segments before you'd need to download and refresh.

**Also added (D2)**: a "buffer nearly full" warning (last 5% of
capacity), surfaced in the web page's buffer-info line, so you get
advance notice to download+refresh rather than silently losing new rows
once the buffer fills. TF card logging is unaffected either way — it
already rotates its own files independently of this RAM buffer.

## What to expect now
- Start → record → Stop → Start again → record more → Stop: **all of it**
  ends up in one downloadable CSV, in order.
- The web page's Start button log line now shows how many rows are
  already sitting in the buffer, so you can see at a glance that nothing
  was cleared.
- Only clicking **Refresh buffer** clears things — same as before, no
  change there.

## If you'd already flashed v5.1.1 and lost data
That data is gone — it was only ever in RAM (unless a TF card was also
present, in which case check the TF card's own per-session CSV files,
which weren't affected by this bug). Nothing to do differently going
forward except: flash this version.

---

# BCMCH Tremor Monitor v5.1.1 — readability + backlight + CSV download fix

Diff on top of v5.1.0 (tagged `R1`–`R3` in the `.ino`, alongside the
existing `T1`–`T10` tags from the previous update — `grep -n "R[0-9]"` or
`grep -n "T[0-9]"` to find them).

## 1. LCD readability (R2, R3)

**Before**: a bare number ("23.4") on the panel — precise, but not
glanceable, especially for a patient checking their own wrist mid-tremor.

**Now**: the panel leads with a large, plain-language, color-coded status
word:

| Word | Color | Range (deg/s, tunable) |
|---|---|---|
| CALM | green | < 25 |
| MILD | yellow-green | 25–60 |
| MODERATE | orange | 60–100 |
| STRONG | red | > 100 |

The precise number ("23.4 deg/s") is still shown, smaller, underneath —
so nothing clinically useful is lost, it's just no longer the primary
thing you have to read. "Min"/"Max" became "Lowest"/"Highest" (plainer
English), each on more generously spaced text.

**Important**: those four thresholds are a reasonable starting point, not
a clinically validated scale. Tune `TREMOR_BAND_MILD_DPS` /
`_MODERATE_DPS` / `_STRONG_DPS` near the top of the DSP section against
real recorded sessions before relying on the color/word for anything
beyond a rough at-a-glance indicator — the same caution that applied to
the raw severity number in the first place.

**On font size**: the status word opportunistically uses the largest
Montserrat size actually compiled into your `lv_conf.h` (48→40→32→28,
checked via LVGL's own `LV_FONT_MONTSERRAT_*` feature-flag macros), and
falls back to the default theme font if none of those are enabled — so
this **cannot fail to compile** regardless of your `lv_conf.h`, but you
may not get a dramatically bigger word if your project only has the
default font size built in. If the status word looks the same size as
before after flashing, check `lv_conf.h` for `LV_FONT_MONTSERRAT_32` (or
similar) set to `1`, and rebuild.

## 2. Backlight timing (R1)

`BACKLIGHT_TOUCH_ON_MS` (on-time after each touch) changed from **3000
→ 15000**. Worth noting: your original description said "5 seconds,"
but the actual firmware constant was 3000ms — I flagged this discrepancy
back when I first reviewed the sketch. I also bumped
`BACKLIGHT_BOOT_ON_MS` (initial on-time after boot) from 5000 → 15000 to
match, so the behavior is consistent whether the screen just turned on
from boot or from a touch. Change either constant independently if you'd
rather they differ.

## 3. CSV download fix

I found (and fixed) two real bugs in `index.html`, plus added
diagnostics so if it's *still* broken after this, the Log panel will
tell us exactly where:

**Bug 1 — concurrent GATT operations.** The page polls the `STATUS`
characteristic every 3 seconds in the background. Web Bluetooth throws
an error if two read/write operations on the same device overlap in
flight — so if that background poll happened to fire at the same moment
as the download's own write+read sequence, one of them could fail
outright. Fixed by routing **every** GATT read/write through a single
serializing queue (`gattOp()`), so nothing ever overlaps, and by having
the status poll skip itself entirely while a download is active.

**Bug 2 — unsafe buffer slicing.** The notification handlers did
`new Uint8Array(event.target.value.buffer)`, which takes the *entire*
underlying `ArrayBuffer` rather than just the bytes belonging to that
specific notification (`DataView.buffer` isn't automatically clipped to
`byteOffset`/`byteLength`). In practice this usually happens to work
because the browser typically allocates a fresh, exactly-sized buffer
per notification — but it's not guaranteed, and any case where it doesn't
would corrupt the downloaded CSV silently. Fixed to always respect
`byteOffset`/`byteLength` explicitly.

**Also added**: a 6-second stall watchdog (if no chunk arrives for 6s
mid-download, it gives up cleanly with a clear log message instead of
leaving the UI stuck with a disabled button forever), and much more
detailed logging at every step of the download (request sent, byte count
reported by device, each stall/failure reason) so a future failure is
diagnosable from the on-page Log panel alone.

## If it's still not working after this

Please try again and paste **both**:
1. The Log panel contents from the page (Connect → Start → Stop →
   Download, whatever sequence you tried)
2. The Serial monitor output from the device over the same window,
   particularly any `[BLE][XFER]` lines

That'll tell us definitively whether it's failing on the write, the info
read, or partway through the chunk transfer — and whether the problem is
even still in the software at all versus, e.g., the device being out of
BLE range or the CSV buffer genuinely being empty (check the "CSV
buffer" line in the device-info panel before downloading — if it says
"0 bytes / 0 rows," record something first).

## Files
- `W146_v5_1_0_tremor.ino` — v5.1.0 firmware + the R1–R3 diff (readability, backlight)
- `index.html` — v5.1.0 web page + the CSV download fixes
