# Cello/piano classifier: design handoff

This document explains how the board decides whether a practice session was **cello or
piano**, why it is built this way, how it learns, how it is tested, and where it is weak.
It is written for whoever improves it next. Every number here matches the code in
`main/classifier.c`, `main/detector.c` and `main/main.c` on `main` at the time of writing.

## Status in one paragraph

The classifier is a small, hand-designed model in plain C (no ML framework). It measures a
few physical properties of each note and compares them with two learned instrument
profiles. It has been tested **only on synthetic audio** (see [Testing](#testing)). It has
**not** been validated on a real cello and piano. `AGENTS.md` notes that the family's board
was running a different firmware (`piano_monitor` v0.1.1) as of 2026-11-04, so field
observations from that board are not evidence about this code. Real recordings are the most
valuable next step.

## Constraints that shaped the design

| Constraint | Consequence |
|---|---|
| Runs on an ESP32-S3 next to an LVGL UI, on one core | No FFT-heavy front end, no neural network. Everything is per-hop arithmetic on a 64 ms frame. |
| Must work from day one with no training data | Instrument profiles start from physics-based defaults. |
| Must adapt to one family's instruments and room | The profiles are a tiny model (two centroids + an hour histogram) learned on the device. |
| Never both instruments at once | The decision is per **session**, not per note, so evidence can add up over minutes. |
| The user should never have to touch it | Learning from its own confident decisions is allowed, but cautiously. |
| Easy to test without hardware | Detector, session logic and classifier are pure C with no ESP-IDF dependencies. |

## Where the code is

| File | Role |
|---|---|
| `main/detector.c/.h` | Audio front end: level, noise floor, YIN pitch, brightness, "is music playing". |
| `main/classifier.c/.h` | Notes, features, votes, session decision, learning rule. |
| `main/session.c/.h` | Session start/stop state machine (independent of the classifier). |
| `main/main.c` | Wiring: when to reset/feed/decide, user overrides and corrections, when to learn. |
| `main/store.c` | Saves and loads the learned model (NVS blob `model`). |
| `test/classify_test.c` | Synthetic cello/piano generator and the classifier tests. |

## Pipeline

```
mic 16 kHz ─► decimate to 8 kHz ─► detector, every 32 ms hop:
                                     level dB, noise floor, f0 (YIN), harmonicity,
                                     brightness, "musical" flag, activity
        │
        ▼
classifier, every hop while a session is open:
   note segmentation ─► per-note features ─► per-note vote (logit)
        │
        ▼
   session evidence = Σ votes ─► + time-of-day prior ─► p(piano) ─► label + confidence
        │
        ▼ (session ends; correction window)
   learning: move the label's profile toward this session's features
```

### 1. Detector (`detector.c`)

* Input: 16 kHz mono, decimated 2:1 with a [1 2 1]/4 filter to 8 kHz.
* Frame 512 samples (64 ms), hop 256 (32 ms).
* `level_db`: RMS of the DC-removed frame, dBFS.
* `floor_db`: adaptive noise floor. Falls fast toward quieter frames (20 %/hop). Rises
  ~1 dB/s on quiet frames, ~0.1 dB/s on loud non-pitched frames, never on pitched frames.
  Starts at −70 dBFS.
* `f0`, `harmonicity`: YIN with threshold 0.15, lag 8–128 samples (1000–62.5 Hz; cello C2 is
  65 Hz). Only computed when the frame is loud: level > floor + sensitivity (default 15 dB,
  user slider) and > −65 dBFS. `f0` is reported only when harmonicity ≥ 0.70.
* `bright_db`: 10·log10(energy of the first difference / energy). A crude spectral-tilt
  measure: it falls when upper partials die away.
* A hop is *musical* if it is loud, harmonic, and its pitch has been stable (within 4 %,
  octave-folded) for 2 hops. *Active* = ≥ 30 % musical hops over the last ~3 s. This drives
  session start/stop only; the classifier uses `f0`, `level_db`, `floor_db`, `bright_db`.

### 2. Note segmentation (`cls_push`)

* A **note** is a run of hops with `f0 > 0` whose pitch stays within **4.5 %** of the note's
  first pitch (octave errors folded back). A pitch move or an unpitched hop ends the note.
* Notes shorter than **5 hops** (160 ms) are ignored.
* At **48 hops** (~1.5 s) a long note is scored and a new segment starts.
* For within-note measurements, **2 hops** are trimmed at each end (transitions).
* **Re-strike check:** if level jumps > **2.5 dB** between hops after the note's peak, the
  pitch tracker probably merged several notes (fast scales locking onto a common period).
  Slope, darkening and vibrato are then not measured for that note.

### 3. Per-note features (`finish_note`)

| Feature | How it is measured | Piano | Cello | Valid when |
|---|---|---|---|---|
| **slope** (dB/s) | Least-squares slope of level from the peak (loudest of the first 4 hops) to the trimmed end | Clearly negative: a struck string only decays | ~0: a bow holds or swells | No re-strike, ≥ 5 body hops |
| **darken** (dB/s) | Least-squares slope of `bright_db` over body hops that are ≥ 12 dB above the floor | Negative: high partials die first, **even with the sustain pedal** | ~0: the bow keeps feeding all partials | As slope, plus ≥ 5 such hops covering ≥ 2/3 of the body |
| **attack** (dB) | Peak of the first 3 hops minus the quietest of the 3 hops just *before* the note (includes the previous note's tail) | Hammer jump | Soft bow change | Always |
| **pitch** | log2 of the median f0 | — | — | Always |
| **vibrato** (cents) | Detrended pitch deviation (RMS, cents) × a regularity factor. The factor is `clamp((−r_min − 0.3)/0.4, 0, 1)`, where `r_min` is the most negative autocorrelation at lags 2–4 hops (4–8 Hz). Real vibrato is periodic (r ≈ −1); piano beating and tracker noise are not. | ~0 | Often 10–25 | No re-strike, ≥ 8 trimmed body hops |

Clamp ranges (also used to validate a stored model): slope −40…15, darken −60…20,
attack 0…30, pitch 5.5…10.5, vibrato 0…40.

### 4. Per-note vote (`cls_note_logit`)

A logit, positive means piano:

1. **Profile distance** over slope, darken and pitch only:
   `d_k = Σ weight_f · ((x_f − centroid_k,f) / scale_f)²`
   with scale = 6, 3, 1 and weight = 1, 1, 0.15 respectively.
   Distance term = `0.5 · (d_cello − d_piano)`.
2. **Vibrato, one-sided:** `−1.5 · clamp((vibrato − 7) / 6, 0, 1)`.
   Vibrato is strong cello evidence, but its *absence* proves nothing (scales and etudes are
   played straight), so it never votes for piano.
3. **Attack, one-sided:** `+1.0 · clamp((attack − 5) / 5, 0, 1)`.
   A hammer-like jump is piano evidence; a soft start proves nothing (fast legato piano
   barely rises over the still-ringing previous note).
4. Clamp the note's vote to **±1.5**, so no single note decides a session.
5. Weight by note length: `w = clamp((hops − 4) / 12, 0.25, 1)`. Long notes are measured
   better.

Attack and vibrato have weight 0 in the distance on purpose; they only act through their
one-sided rules. Their centroids are still stored and learned, for future use.

### 5. Session decision (`cls_p_piano`, `cls_decide`)

```
p(piano) = sigmoid( 0.5 · Σ_notes w·vote  +  time_prior(hour) )
time_prior(h) = clamp( ln((piano_sessions[h] + 1) / (cello_sessions[h] + 1)), −1.5, +1.5 )
```

* Label = piano if p ≥ 0.5. Confidence = max(p, 1 − p).
* `hour` is the local hour when the session started (needs the SNTP clock; −1 = no prior).
* The UI shows the label as settled after **8 notes** (`IDENTIFY_NOTES` in `ui.c`).
* Evidence is reset when a new session becomes a candidate (`main.c`); every hop of an open
  session is fed, including pauses. There is one label per session; switching instruments
  without a 90 s silence gives one mixed session (a known limit).

### 6. Learning (`apply_learning` in `main.c`, `cls_learn`)

A finished session stays **pending** for the correction window (default 15 min,
`CONFIG_PL_CORRECT_WINDOW_MIN`) or until the next session starts. Then:

| How the label was obtained | Profile learning rate | Updates the hour histogram |
|---|---|---|
| User set it during the session, or corrected it afterwards | 0.3 | yes |
| User tapped the label it already had (confirmation) | 0.2 | yes |
| Automatic, confidence ≥ 0.95 | 0.1 | yes |
| Automatic, confidence 0.8–0.95 | 0 | yes |
| Automatic, confidence < 0.8 | 0 | no |

* Profile update = exponential moving average of the labelled instrument's centroid toward
  the session's **mean** feature vector, per feature, only for features measured on ≥ 5
  notes, clamped to the ranges above.
* Hour histogram: +1 for the label at that hour. When any bin reaches 1000, all bins halve.
* The model is about 140 bytes: magic + 2 × 5 centroids + 2 × 24 hour counts. It is saved to
  NVS (`practice` namespace, blob `model`) and loaded at boot. It is rejected and replaced by
  defaults if the magic differs or any centroid is outside its range or NaN.

### Default profiles

| | slope dB/s | darken dB/s | attack dB | pitch | vibrato cents |
|---|---|---|---|---|---|
| Cello | 0 | 0 | 2 | 7.4 (≈170 Hz) | 10 |
| Piano | −12 | −4 | 8 | 8.2 (≈290 Hz) | 1 |

## Testing

`make -C test` builds and runs two host programs, no ESP-IDF needed.

`test/classify_test.c` synthesizes instruments sample by sample:

* **Bowed voice:** 1/h partials, 30–120 ms bow attack, slow ±25 % pressure swell, a random
  walk of bow pressure (±15 %), vibrato 14–24 cents at 5–6.5 Hz after 150 ms (or none),
  ±2 cent pitch jitter, 4 % bow noise.
* **Piano voice:** partials 1/(0.6h + 0.4), 4 ms attack, two-stage decay (e.g. 18 then
  6 dB/s; 8 then 3 dB/s for low notes with pedal), higher partials decaying faster
  (an extra 0.35·(h − 1) × the decay in dB), ±1.5 dB beating at 0.5–2 Hz.
* **Styles:** cello with vibrato, without vibrato, fast détaché; piano melody, chords
  (3 notes + melody), fast scales, low notes with pedal. Quiet room and a noisy room
  (≈ −35 dBFS).

Results on `main`:

| Test | Result |
|---|---|
| Each style, 90 s, quiet and noisy room | All cello and piano styles right within 30 s, except below |
| Fast piano scales only | Often labelled cello: very short notes carry little evidence (reported, not gated) |
| Fast cello détaché only | Right in these runs, but on weak evidence (reported, not gated) |
| 4-minute mixed sessions, 5 seeds × 2 rooms | All correct |
| Scales-only piano, after user corrections | Right in 3–5 of 8 seeds after 3 corrections, 7 of 8 after 6 |
| Time prior alone / against clear audio | Leans the right way; audio overrides it |

Two caveats about these numbers. The synthetic signals are built from the same physical
ideas the features measure, so passing them proves the code does what was intended, not that
real instruments behave that way. And single-seed results can be lucky: an earlier
"3 corrections fix scales-only piano" claim turned out to depend on one random sequence.
Check rates over several seeds.

## Known weaknesses

| Weakness | Evidence | Status |
|---|---|---|
| **Plucked cello reads as piano.** Tuning by plucking open strings (typically the day's first cello session) or pizzicato fades and darkens exactly like piano. After ~25 s of plucking, bowing needs 17–27 s to win. | Synthetic tuning test | Fix proposed on branch `fix/cello-tuning-reads-as-piano`: a note that holds level and colour (or has clear vibrato) can only be bowed; once ≥ 4 notes and ≥ 15 % of notes are like that, the session is decided for cello. Piano makes ≤ 2 such notes a minute, bowed cello ~40. Bowing then wins in 6–19 s; nothing else changes. |
| Sessions of only very short notes (fast scales, fast détaché) | Synthetic tests | Corrections help, slowly (see above). |
| One label per session | By design | Switching instruments within 90 s merges them. A change-point detector on the evidence stream would fix it. |
| Self-training can reinforce its own mistakes | By design (rate 0.1 at ≥ 0.95 confidence; hour histogram at ≥ 0.8) | Simulated only with correctly labelled sessions. A confident mistake that nobody corrects moves the wrong profile; watch for drift on real data. |
| Profiles learn means, not spreads | `cls_learn` | The fixed scales (6, 3, 1) may not fit real instruments or rooms. |
| Recorded music counts as playing | Detector design | Unaddressed. |
| No real-world validation | — | The most important gap. |

## Ideas for the next person, roughly in order of value

1. **Collect real data.** Log each note's features, the session label, confidence and any
   correction (over serial or into the SPIFFS CSV). A week of the family's practice would
   show which features separate the real instruments and how wide each instrument's spread is.
2. **Add a WAV replay harness.** Feed recorded audio through `det_push` → `cls_push` on the
   host, so real recordings become regression tests, like the synthetic ones.
3. **Learn spreads, not just centres.** Replace fixed scales with per-instrument variances
   (a diagonal Gaussian per instrument), updated with the same rates. Keep the clamps.
4. **Merge the tuning fix** after checking it on real tuning and pizzicato recordings.
5. **Detect instrument changes inside a session.** For example, a CUSUM on the per-note votes.
6. **Calibrate confidence** on real data, so 90 % means right 9 times in 10. The learning
   thresholds (0.8, 0.95) depend on it.
7. **A learned model (TinyML)** is possible later. Keep the same interface (`cls_push`
   per hop, `cls_p_piano` per session) so the session logic, UI, correction flow and storage
   stay unchanged.

## Rules when changing the model

* **Bump `CLS_MODEL_MAGIC`** whenever features, their order or `cls_model_t` change.
  Otherwise a stored model is loaded with the wrong meaning. A bump resets learning to the
  defaults on the next boot.
* Keep the clamp ranges (`LO`/`HI`); they also guard against a corrupted stored model.
* Keep ambiguous cues one-sided. A missing cue (no vibrato, a soft attack) must not count as
  evidence for the other instrument.
* Keep `classifier.c` free of ESP-IDF dependencies so it stays testable on a PC.
* `make -C test` must pass. Add a test for each failure mode you fix, and check it fails
  without your fix. Prefer rates over several random seeds to single runs.
