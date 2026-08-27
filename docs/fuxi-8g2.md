# Fuxi / Snapdragon 8 Gen 2 tuning profile

This profile targets the Fuxi-class Snapdragon 8 Gen 2 topology observed on the test device:

| CPUs | capacity | max frequency | semantic tier |
|---|---:|---:|---|
| 0-2 | 280 | 2.016 GHz | e-core |
| 3-6 | 855 | 2.8032 GHz | p-core |
| 7 | 1024 | 3.1872 GHz | hp-core |

## Recommended policy

- Keep unlisted applications on the dynamic load balancer.
- Use `p-core` for latency-sensitive UI threads such as `RenderThread` rather than pinning the whole application to `hp-core`.
- Reserve `hp-core` for a small number of genuinely latency-sensitive threads, especially the top load thread of games.
- Keep the rest of an application on `e-core,p-core` so EAS can still balance work across four performance cores and three efficiency cores.

## Why this is not a direct 8G3 port

The Comet 8G3 rules contain literal CPU ranges such as `5-6`, `2-6`, and `2-7`. Those ranges describe the author's target topology and should not be blindly reused on Fuxi. This profile uses AkiAppOpt's semantic tiers instead: `e-core`, `p-core`, `hp-core`, and `all-core`.

## Suggested first test

Start with dynamic load balancing and only a small number of explicit rules:

```text
com.miui.home{RenderThread}=p-core
com.android.systemui{RenderThread}=p-core
```

For games, enable the game list and let the load balancer place the highest-load thread on `hp-core` and the second-highest on `p-core`.

Do not disable EAS or globally pin normal applications to CPU 7 based on benchmark results alone. Validate frame-time/jank, temperature, and sustained performance against the stock configuration.
