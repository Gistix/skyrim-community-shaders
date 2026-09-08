# Measuring generated-frame pacing

Frame rate does not tell you whether generated frames are *paced*. A generator can present twice
as many frames and deliver no extra smoothness if each generated frame lands next to its real
frame instead of halfway between them. The overlay cannot see this: it reports render frame
time, which stays smooth while the delivered cadence is ragged.

PresentMon reports the cadence directly. `capture-pacing.ps1` loads the standard bench scene,
settles it, and captures 20 s of presents; `pmstats.awk` reduces the CSV.

```
capture-pacing.ps1 -Label dlssg -Settings SettingsUser.dlssg_unlocked.json
awk -F, -f pmstats.awk pm_dlssg.csv   # rate, deviation, bunching
awk -F, -f hist.awk    pm_dlssg.csv   # deviation histogram + sd excluding the worst 1%
awk -F, -f seg.awk     pm_dlssg.csv   # the same split into four time segments
```

## Read MsBetweenDisplayChange, not MsBetweenPresents

This is the trap, and it is easy to draw exactly the wrong conclusion from it.

`MsBetweenPresents` is when the application *called* present. `MsBetweenDisplayChange` is when
the frame actually reached the screen. A frame generator that meters flips in software submits
its two presents together and spaces them afterwards, so the two columns disagree completely:

|  | present intervals | display changes |
| --- | --- | --- |
| FSR-FG, uncapped | sd 0.39 ms, 0.0% under 1 ms | sd 0.51 ms, 0.0% under 1 ms |
| DLSS-G, uncapped | sd 3.39 ms, **49.7%** under 1 ms | sd 1.02 ms, **1.1%** under 1 ms |

Judged on presents alone DLSS-G looks completely unpaced. Judged on what is scanned out it is
paced, just about twice as loosely as FFX. Only the second column describes what a player sees.

## Cadence is correct, and best where it matters

Uncapped at 279 fps the display cannot give each frame its own scanout, so some variation there
is expected and not very meaningful. Capped -- how the game is actually played -- DLSS-G is
exact:

```
display changes 595 | mean 33.35 ms | sd 0.01 ms | 0% bunched
  min 33.31 ms  max 33.40 ms  100% of intervals in a single 8 ms bucket
```

Locked to the target with a hundredth of a millisecond of deviation. FSR-FG capped is not this
tight -- see "FSR-FG carries residual jitter" below.

## What that means for the two generators

**FFX (FSR-FG)** owns its present loop and spaces the generated frame itself, so both columns
agree and the cadence is tight.

**DLSS-G** emits its generated frame from inside the same present call as the real one — the same
property that forces `GetRenderedFrameRateLimit` to hand it the frame limit undivided — and then
meters the flip in software. On Ada that metering is software, not the hardware flip metering
Blackwell added.

Nothing is dropped at the DXGI layer in either case.

## FSR-FG carries residual jitter that DLSS-G does not

Capped, the two generators differ sharply in delivered cadence:

| capped | display sd | min | max |
| --- | --- | --- | --- |
| DLSS-G | **0.01 ms** | 33.31 | 33.40 |
| FSR-FG | **2.65 ms** | 28.92 | 67.33 |

FSR-FG holds target on 97.6% of frames, but 2.2% arrive late and 0.5% are doubled outright --
a visible hitch every six seconds or so. DLSS-G never misses.

The difference is which pacer is in charge. FFX paces by blocking the caller in `Present` (see
below), so its cadence is whatever its own internal pacer produces and nothing upstream of the
present can improve it. DLSS-G is handed the OUTPUT target and meters its flips downstream of the
game's present, which is why it can hit a hundredth of a millisecond. Neither is reachable from CS
or DXVK, but only one of them has a defect.

Splitting the deviation shows two separate faults rather than one. Removing the worst 1% of
intervals:

| capped | sd | sd excl. worst 1% | intervals >10 ms off |
| --- | --- | --- | --- |
| DLSS-G | 0.013 ms | 0.013 ms | 0% |
| FSR-FG | 2.65 / 2.16 / 1.82 ms | 1.00 / 0.82 / 0.81 ms | 1.0% / 0.3% / 0.5% |

A continuous sub-millisecond phase spread, plus a rare dropped frame that dominates the untrimmed
figure. The tail is what a player notices; the spread is what looked addressable, and was not.


## What actually paces the FSR-FG loop

Worth stating plainly, because two comments in the source used to imply otherwise: on the FSR-FG
path **FidelityFX paces the loop itself, by blocking inside `Present`**. Reflex's `frameLimitUs`
is handed to it and does nothing there.

Instrumenting a present gate made this unambiguous. Gating the game's `Present` call onto a fixed
66.7 ms grid, the gate reported waiting on every single frame:

```
[Pace] 300 frames | waited 300 (58.04 ms avg) | late 0 | re-anchored 0 | arrival vs grid -58.06 ms avg
```

58 ms of wait in a 66.7 ms frame. The render loop's own work is only ~8.7 ms; the other 58 ms was
never Reflex sleeping and never the loop being slow -- it was FFX holding the caller until it was
ready for the next real frame. The gate did not add pacing, it *relocated* FFX's pacing wait to
before the present call.

## Tried and rejected: gating the present

That relocation is harmful, for a reason that is obvious once the mechanism is right: arriving at
FFX exactly when it wants the next frame leaves it no slack to place the interpolated one. The
delivered cadence splits systematically instead of jittering -- in one run 63.8% of intervals sat
2-4 ms off the mean, against 3.5% ungated. It also costs up to 58 ms of latency, since the wait
happens with a fully rendered frame in hand.

Four runs, deviation excluding the worst 1% of intervals:

```
gate off   1.39 / 4.35 ms
gate on    3.04 / 0.43 ms
```

The 0.43 ms run is real and is the reason this was worth chasing -- segmented, it held sd 0.40 /
0.47 / 0.56 ms over fifteen seconds, near DLSS-G's 0.013 ms. But it is a phase accident: the gate
happened to land in step with FFX's own pacer. Adding 2% headroom to the Reflex cap to make the
lock reproducible did not (that run went to 3.12 ms), because the headroom was addressing a slow
render loop that does not exist.

## The bench is noisier than one run can show

Every conclusion on this page that rests on a single capture should be distrusted. Across six
capped FSR-FG runs of the same scene with no code change at all, deviation excluding the worst 1%
of intervals ranged 0.81 to 4.35 ms, and the untrimmed figure ranged 1.82 to 6.85 ms. Two separate
changes here produced a convincing first result that reversed on pairing.

Segment each capture before trusting it -- `seg.awk` splits it into quarters. The first quarter is
routinely several times worse than the rest even after a 60 s settle, so a capture that starts
early reads as a regression that is not there.

## Tried and rejected

Chaining present IDs into `VkPresentInfoKHR` so DLSS-G could use `vkWaitForPresentKHR` for
timing feedback. DXVK had stopped chaining them along with the present fence, but the deadlock
argument for dropping the fence never applied to IDs, since nothing in DXVK waits on one.
Restoring them changed nothing for the better: display sd 1.02 -> 1.11 ms, frames bunched at
scanout 1.1% -> 2.1%, and 3% fewer frames. DLSS-G is not waiting on present IDs here.

Present mode and frame cap were also each measured against DLSS-G's cadence and neither moved it.

Handing FSR-FG the output target undivided, so Reflex paces output the way it does for DLSS-G.
It does not pace better and it breaks the cap: against a 30 fps target it delivered 59.8 fps,
because Reflex then holds the render loop at 30 and FFX still doubles it. Normalised for rate
the jitter is unchanged, 7.9% of the frame interval against 8.4%.

Removing DXVK's frame limiter -- pacing-neutral, 2.66 -> 2.65 ms, because Reflex
was already available so CS had it switched off.

Turning Reflex low-latency mode on for the FSR-FG path. With DXVK's limiter gone, one of the two
reasons for leaving it off went with it, and steadying the render loop is exactly what this
defect wants. It does not steady it. Three paired runs each, display-interval deviation:

```
mode off   2.65 / 2.16 / 1.82 ms    mean 2.21
mode on    2.20 / 3.40 / 2.63 ms    mean 2.74
```

Slightly worse, ranges overlapping. Worth recording how this one presented: the first run of
each showed 2.20 against 2.65 and read as a 17% win in the direction the mechanism predicts.
Pairing reversed it. Single runs on this bench have now produced a false positive three times
in a row, so nothing here should be landed on one.
