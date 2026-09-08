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

## What that means for the two generators

**FFX (FSR-FG)** owns its present loop and spaces the generated frame itself, so both columns
agree and the cadence is tight.

**DLSS-G** emits its generated frame from inside the same present call as the real one — the same
property that forces `GetRenderedFrameRateLimit` to hand it the frame limit undivided — and then
meters the flip in software. On Ada that metering is software, not the hardware flip metering
Blackwell added.

Nothing is dropped at the DXGI layer in either case.

## Tried and rejected

Chaining present IDs into `VkPresentInfoKHR` so DLSS-G could use `vkWaitForPresentKHR` for
timing feedback. DXVK had stopped chaining them along with the present fence, but the deadlock
argument for dropping the fence never applied to IDs, since nothing in DXVK waits on one.
Restoring them changed nothing for the better: display sd 1.02 -> 1.11 ms, frames bunched at
scanout 1.1% -> 2.1%, and 3% fewer frames. DLSS-G is not waiting on present IDs here.

Present mode and frame cap were also each measured against DLSS-G's cadence and neither moved it.
