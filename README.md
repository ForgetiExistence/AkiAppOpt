# AkiAppOpt

A lightweight Android application/thread CPU affinity optimizer with semantic CPU tiers and dynamic load balancing.

## CPU tiers

AkiAppOpt detects CPU performance tiers from the device's `cpu_capacity` information when available, with frequency-based fallback. Rules can therefore use semantic names instead of hard-coded CPU numbers:

- `e-core` — lowest-capacity CPUs
- `p-core` — high-performance CPUs
- `hp-core` — highest-capacity CPU(s)
- `all-core` — all present CPUs

This is important for devices with different big.LITTLE layouts. A rule written for one Snapdragon generation should not be assumed to have the same meaning on another generation.

## Fuxi / Snapdragon 8 Gen 2

For the tested Fuxi topology:

```text
CPU 0-2: capacity 280,  max 2.016 GHz  -> e-core
CPU 3-6: capacity 855,  max 2.8032 GHz  -> p-core
CPU 7:   capacity 1024, max 3.1872 GHz  -> hp-core
```

A Fuxi-specific tuning guide is available at `docs/fuxi-8g2.md`.

The recommended approach is to leave most applications to the dynamic load balancer and explicitly place only latency-sensitive threads (for example `RenderThread`) on `p-core`. Reserve `hp-core` for a small number of genuinely latency-sensitive threads instead of pinning whole applications to it.

## Configuration

The module reads `applist.conf` and `gamelist.conf` from its module directory. Changes to rules can be reloaded without rebooting. Semantic tiers may be combined with numeric CPU ranges using commas.

Example:

```text
com.example=all-core
com.example{RenderThread}=p-core
```

## Testing guidance

Affinity changes should be evaluated against stock scheduling using frame-time/jank, temperature, sustained performance, and power—not benchmark score alone.
