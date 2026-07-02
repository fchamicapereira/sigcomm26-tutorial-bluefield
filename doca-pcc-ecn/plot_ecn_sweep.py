#!/usr/bin/env python3
"""SIGCOMM-tutorial figure: pure-ECN (DCQCN-style) controller on BF3 DPA.
Sweep of the per-CNP multiplicative-decrease factor, 2->1 RoCE incast into a 100G sink."""
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter

# --- measured data (2->1 incast, Q3 lossless+ECN, tclass=104) ---
factor  = np.array([0.995, 0.99, 0.98, 0.95, 0.90, 0.85, 0.80])
cut_pct = (1.0 - factor) * 100.0                      # per-CNP cut, %
bw_A    = np.array([21.83, 33.82, 40.04, 43.37, 44.77, 43.62, 43.88])
bw_B    = np.array([22.52, 34.03, 39.71, 43.62, 43.76, 44.51, 42.80])
bw_agg  = bw_A + bw_B
cnp     = np.array([123845, 63099, 31080, 11958, 5600, 3513, 2617])
nack    = np.array([130085, 67066, 34388, 14927, 7774, 5097, 3822])
rtt_us  = np.array([56.5, 30.9, 53.8, 32.3, 17.2, 20.9, 6.3])         # log-only, small-sample (noisy)

HYBRID_BW = 86.0        # RTT+ECN hybrid reference
peak_i    = int(np.argmax(bw_agg))

plt.rcParams.update({"font.size": 11, "font.family": "DejaVu Sans",
                     "axes.grid": True, "grid.alpha": 0.3, "axes.axisbelow": True})
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(7.2, 7.6), sharex=True)

# ---------- (a) goodput U-curve ----------
ax1.axhline(HYBRID_BW, ls="--", lw=1.2, color="grey", zorder=1)
ax1.text(0.55, HYBRID_BW + 0.8, "RTT+ECN hybrid (86G)", color="grey", fontsize=9)
ax1.plot(cut_pct, bw_agg, "-o", color="#1f77b4", lw=2, ms=7, zorder=3, label="aggregate goodput")
ax1.plot(cut_pct[peak_i], bw_agg[peak_i], "*", color="#d62728", ms=20, zorder=4,
         label=f"peak: x{factor[peak_i]:.2f} -> {bw_agg[peak_i]:.1f} Gb/s")
# regime annotations
ax1.annotate("gentle cut:\ndeep-queue collapse", xy=(cut_pct[0], bw_agg[0]),
             xytext=(0.55, 58), fontsize=9, ha="center", color="#555",
             arrowprops=dict(arrowstyle="->", color="#999", lw=1))
ax1.annotate("over-aggressive:\nslight underutilization", xy=(cut_pct[-1], bw_agg[-1]),
             xytext=(19, 74), fontsize=9, ha="center", color="#555",
             arrowprops=dict(arrowstyle="->", color="#999", lw=1))
ax1.set_ylabel("Aggregate goodput  (Gb/s)")
ax1.set_ylim(38, 96)
ax1.set_title("Pure-ECN controller on BF3 DPA: per-CNP decrease-factor sweep\n"
              "(2$\\rightarrow$1 RoCE incast, 100G lossless sink; all points fair, Jain$\\approx$1.0)",
              fontsize=11)
ax1.legend(loc="lower center", fontsize=9, framealpha=0.9)

# ---------- (b) mechanism: signaling + retransmission load ----------
ax2.plot(cut_pct, cnp,  "-s", color="#ff7f0e", lw=2, ms=6, label="CNP events")
ax2.plot(cut_pct, nack, "-^", color="#2ca02c", lw=2, ms=6, label="NACK (retransmit)")
ax2.set_yscale("log")
ax2.set_ylabel("events per 20 s run  (log)")
ax2.set_xlabel("per-CNP multiplicative cut  (%)   [$\\rightarrow$ more aggressive]")
ax2.legend(loc="upper right", fontsize=9)
# RTT as a light annotation series on a twin axis
ax2b = ax2.twinx()
ax2b.plot(cut_pct, rtt_us, ":d", color="#9467bd", lw=1.4, ms=5, alpha=0.8, label="RTT avg (log-only)")
ax2b.set_ylabel("RTT avg  ($\\mu$s)", color="#9467bd")
ax2b.tick_params(axis="y", labelcolor="#9467bd")
ax2b.grid(False)
ax2b.legend(loc="center right", fontsize=8)

ax2.set_xscale("log")
ax2.set_xticks(cut_pct)
ax2.get_xaxis().set_major_formatter(ScalarFormatter())
ax2.set_xticklabels([f"{c:g}\n(x{f:.3g})" for c, f in zip(cut_pct, factor)], fontsize=8)

fig.tight_layout()
for ext in ("pdf", "png"):
    fig.savefig(f"doca_pcc_ecn_sweep.{ext}", dpi=150, bbox_inches="tight")
print("wrote doca_pcc_ecn_sweep.pdf and .png")
print(f"peak: x{factor[peak_i]:.2f} = {cut_pct[peak_i]:g}% cut -> {bw_agg[peak_i]:.1f} Gb/s")
