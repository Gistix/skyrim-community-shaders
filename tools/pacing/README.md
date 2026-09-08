# Measuring generated-frame pacing

Frame rate does not tell you whether generated frames are *paced*. A generator can present
twice as many frames and deliver no extra smoothness, if each generated frame lands next to
its real frame instead of halfway between them. The overlay cannot see this: it reports render
frame time, which stays smooth while the present cadence is ragged.

PresentMon reports the present cadence directly. `capture-pacing.ps1` loads the standard bench
scene, settles it, and captures 20 s of presents; `pmstats.awk` reduces the CSV to the numbers
that matter:

```
capture-pacing.ps1 -Label dlssg -Settings SettingsUser.dlssg_unlocked.json
awk -F, -f pmstats.awk pm_dlssg.csv
```

The two figures to read are the standard deviation of `MsBetweenPresents` and the share of
intervals under 1 ms. A well-paced 2x generator has a low deviation and *no* sub-millisecond
intervals; a high count of those means real and generated frames are being emitted together.

## What it found

Whiterun bench, RTX 4080, 20 s per run:

| | interval sd | back to back | never displayed |
| --- | --- | --- | --- |
| FSR-FG, uncapped (tearing) | **0.39 ms** | **0.0%** | 0% |
| DLSS-G, uncapped (tearing) | 3.39 ms | **49.7%** | 0% |
| DLSS-G, capped (tear-free) | 33.12 ms | **50.1%** | 0% |

**FSR-FG is correctly paced.** FFX owns its present loop and spaces the generated frame itself.

**DLSS-G is not, and the present mode does not change that.** Half its presents land within a
millisecond of the previous one in every configuration tested. Nothing is dropped at the DXGI
layer, so this is not a capacity or throughput problem: the frames are generated and displayed,
just not spread out, which is why 279 fps of DLSS-G output judders.

The cause is structural rather than a misconfiguration. `sl.dlss_g` emits its generated frame
from inside the same present call as the real one -- see the DLSS-G branch of
`Upscaling::GetRenderedFrameRateLimit`, where the same property forces the frame limit to be
handed over undivided. There is no interval between the two presents for a present mode, a
frame cap, or a queue depth to widen. On D3D12 the spacing is done by hardware flip metering,
which has no equivalent on this path.

Everything CS controls was checked and is already correct: Reflex is forced on for DLSS-G
(`GetEffectiveReflex`), all seven PCL markers are emitted, `queueParallelismMode` is the
documented default whose contract CS meets, and the frame limit is handed to the limiter
undivided. Present mode, frame cap, and Reflex limit were each measured and none affects the
cadence.

So on the Vulkan path FSR-FG is the frame generator that actually delivers paced frames.
