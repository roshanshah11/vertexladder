"""Charts for docs/RESEARCH.md from the CSVs that `microstructure --csv` writes.

    python3 analysis/plot.py analysis/results/2019-12-30

Writes <prefix>-light.png and <prefix>-dark.png; the docs pick one with <picture>.
"""
import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e", grid="#e4e3de",
                  s1="#2a78d6", s2="#eb6834"),
    "dark": dict(surface="#1a1a19", ink="#ffffff", ink2="#c3c2b7", grid="#34332f",
                 s1="#3987e5", s2="#d95926"),
}


def read(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def style(ax, t):
    ax.set_facecolor(t["surface"])
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(t["grid"])
    ax.tick_params(colors=t["ink2"], labelsize=9, length=0)
    ax.grid(axis="y", color=t["grid"], linewidth=0.8)
    ax.set_axisbelow(True)


def bars(ax, t, values, fmt, ylabel, title, labels):
    style(ax, t)
    rects = ax.bar(range(len(values)), values, color=t["s1"], width=0.62)
    for r, v in zip(rects, values):
        ax.annotate(fmt.format(v), (r.get_x() + r.get_width() / 2, v), xytext=(0, 3),
                    textcoords="offset points", ha="center", color=t["ink"], fontsize=8)
    ax.set_xticks(range(len(labels)), labels)
    ax.set_xlabel("shares already ahead when the order joins", color=t["ink2"], fontsize=9)
    ax.set_ylabel(ylabel, color=t["ink2"], fontsize=9)
    ax.set_title(title, color=t["ink"], fontsize=11, loc="left")


def main(prefix):
    imb = read(prefix + "_imbalance.csv")
    queue = read(prefix + "_queue.csv")
    labels = ["0\n(new level)", "1-499", "500-\n1,999", "2,000-\n9,999", "10,000+"]
    for name, t in THEMES.items():
        fig, axes = plt.subplots(1, 3, figsize=(13, 4.2), dpi=150)
        fig.patch.set_facecolor(t["surface"])

        ax = axes[0]
        style(ax, t)
        for cls, color, label in (("one_tick", t["s1"], "spread = 1 tick"), ("wider", t["s2"], "spread > 1 tick")):
            rows = [r for r in imb if r["spread_class"] == cls]
            x = [(float(r["bucket_lo"]) + float(r["bucket_hi"])) / 2 for r in rows]
            y = [100 * float(r["p_up"]) for r in rows]
            ax.plot(x, y, color=color, linewidth=2, marker="o", markersize=6,
                    markeredgecolor=t["surface"], markeredgewidth=1.5, label=label)
            ax.annotate(f"{y[-1]:.0f}%", (x[-1], y[-1]), xytext=(7, 0), textcoords="offset points",
                        color=t["ink"], fontsize=8, va="center")
        ax.axhline(50, color=t["ink2"], linewidth=0.8, linestyle=(0, (3, 3)))
        ax.set_xlim(-1.05, 1.2)
        ax.set_ylim(15, 85)
        ax.set_xlabel("queue imbalance (bid - ask) / (bid + ask)", color=t["ink2"], fontsize=9)
        ax.set_ylabel("next mid move is up (%)", color=t["ink2"], fontsize=9)
        ax.set_title("Imbalance predicts the next tick", color=t["ink"], fontsize=11, loc="left")
        leg = ax.legend(loc="upper left", fontsize=8, frameon=False)
        for txt in leg.get_texts():
            txt.set_color(t["ink2"])

        bars(axes[1], t, [100 * float(r["fill_rate"]) for r in queue], "{:.1f}%",
             "orders that ever fill (%)", "Behind a long queue, you rarely fill", labels)
        bars(axes[2], t, [100 * float(r["share_underwater_10s"]) for r in queue], "{:.0f}%",
             "fills underwater 10 s later (%)", "...and those fills are worse", labels)
        axes[2].set_ylim(0, 85)

        fig.text(0.01, 0.01, "NASDAQ TotalView-ITCH 5.0, Dec 30 2019, 9:30-16:00 ET, all 8,892 symbols. "
                 "Underwater = mid moved past the fill price against the resting order; fills made while the "
                 "spread was one tick.", color=t["ink2"], fontsize=8)
        fig.tight_layout(rect=(0, 0.04, 1, 1))
        out = f"{prefix}-{name}.png"
        fig.savefig(out, facecolor=t["surface"])
        plt.close(fig)
        print("wrote", out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "analysis/results/2019-12-30")
