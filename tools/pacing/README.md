# Measuring generated-frame pacing

Frame rate does not tell you whether generated frames are *paced*. A generator can present twice
as many frames and deliver no extra smoothness if each generated frame lands next to its real
frame instead of halfway between them. The overlay cannot see this: it reports render frame
time, which stays smooth while the delivered cadence is ragged.

PresentMon reports the cadence directly. `capture-pacing.ps1` loads the standard bench scene,
settles it, and captures 20 s of presents; `pmstats.awk` reduces the CSV.

```
capture-pacing.ps1 -Label dlssg -Settings SettingsUser.dlssg_unlocked.json
awk -F, -f pmstats.awk pm_dlssg.csv
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

The difference is which rate is paced. Reflex holds FSR-FG's RENDER loop and FFX derives the
presented cadence from it, so render-loop jitter lands directly on the delivered frame. DLSS-G
is handed the OUTPUT target and derives its own render cadence, which absorbs that jitter.
FFX's pacing is inside FidelityFX and not reachable from here.

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
