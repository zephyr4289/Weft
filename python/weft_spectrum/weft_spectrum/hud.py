# weft_spectrum/hud.py — WeftSpectrumHud terminal lane (P5, mandate E).
#
# Drop-in diagnostic overlay for Python hosts (notebooks, data pipelines,
# simulators). Mirrors the React/Flutter/SwiftUI HUD contract: renders from
# the caller-owned flyweights only, never mutates their owner state, never
# throws (Law 4 — hostile field values degrade to '-'), diagnostic rate.
#
# The steady-state governor/telemetry path (cadence_tick) is the Law 1 hot
# path and stays allocation-free; THIS painter runs at the diagnostic rate
# (~10 Hz) where line strings are required by the terminal medium (same
# accepted pattern as the JS HUD's textContent rows).

from .wire import (
    TIER_UNKNOWN, TIER_FLAGSHIP, TIER_MID, TIER_BUDGET,
    VIS_VISIBLE, VIS_HIDDEN, VIS_UNKNOWN, ERROR_NAMES,
    E_HUD_CONTEXT_LOST, E_HUD_RECOVERED,
)

_TIER_NAMES = ("UNKNOWN", "T1 FLAGSHIP", "T2 MID", "T3 BUDGET")
_THERMAL_NAMES = ("nominal", "light", "moderate", "severe", "critical")
_VIS_NAMES = {VIS_VISIBLE: "visible", VIS_HIDDEN: "HIDDEN", VIS_UNKNOWN: "unknown"}
_LABELS = ("TIER", "FEATURES", "FPS", "JITTER p99/p99.9", "MEM", "THERMAL", "CADENCE", "STATUS")


class SpectrumHudTerminal:
    """Preallocated row slots; paint() rewrites them in place."""

    def __init__(self, width=64, use_ansi=True):
        self.width = width
        self.use_ansi = use_ansi
        self.rows = {label: "-" for label in _LABELS}
        self.healthy = True
        self.banner = ""

    def paint(self, state, gov=None, metrics=None):
        """One diagnostic frame. Returns 0 healthy, E_HUD_CONTEXT_LOST degraded."""
        try:
            tier = state.siliconTier if 0 <= state.siliconTier <= TIER_BUDGET else TIER_UNKNOWN
            self.rows["TIER"] = _TIER_NAMES[tier]

            flags = (state.featureFlagsHi << 32) | state.featureFlagsLo
            names = []
            for bit, name in sorted(
                    ((b, n) for n, b in _FEATURE_ITEMS()), key=lambda x: x[0]):
                if (flags >> bit) & 1 and len(names) < 3:
                    names.append(name)
            count = bin(flags).count("1")
            self.rows["FEATURES"] = ",".join(names) + (f" +{count - 3}" if count > 3 and names else "")

            if metrics is not None:
                self.rows["FPS"] = str(int(metrics.fps)) if metrics.fps >= 0 else "-"
                self.rows["JITTER p99/p99.9"] = f"{int(metrics.jitterP99Us)}/{int(metrics.jitterP999Us)}us"
                self.rows["MEM"] = f"{round(metrics.memBytes / 1048576)} MiB"
            else:
                self.rows["FPS"] = "-"
                self.rows["JITTER p99/p99.9"] = "-"
                self.rows["MEM"] = "-"

            self.rows["THERMAL"] = _THERMAL_NAMES[state.thermalState] if 0 <= state.thermalState <= 4 else "unknown"
            self.rows["CADENCE"] = f"{gov.capHz} Hz" if gov is not None else "-"

            bat = (f" bat {round(state.batteryPermille / 10)}%"
                   if state.batteryPermille != 0xFFFF and state.batteryPermille <= 0xFFFF else "")
            self.rows["STATUS"] = _VIS_NAMES.get(state.visibility, "unknown") + bat

            if not self.healthy:
                self.healthy = True
                self.banner = ""
                return E_HUD_RECOVERED
            return 0
        except Exception as err:  # Law 4: degrade, never raise into the host
            self.healthy = False
            code = getattr(err, "spectrumCode", E_HUD_CONTEXT_LOST)
            self.banner = f"SPECTRUM HUD FALLBACK: {ERROR_NAMES.get(code, str(code))}"
            return E_HUD_CONTEXT_LOST

    def render(self):
        """Build the overlay text (diagnostic rate — allocations accepted)."""
        lines = []
        if self.banner:
            lines.append(self.banner)
        for label in _LABELS:
            lines.append(f"{label:<16} {self.rows[label]}")
        if self.use_ansi:
            return "\n".join(lines)
        return "\n".join(lines)


def _FEATURE_ITEMS():
    # Local import avoids a module cycle; wire.FEAT is the frozen table.
    from .wire import FEAT
    return FEAT.items()
