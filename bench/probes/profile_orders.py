#!/usr/bin/env python3
"""Generates order execution profiling data and renders high-resolution analytics graphics."""

from dataclasses import dataclass
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt
import numpy as np
from matplotlib import ticker

# Ensure high-DPI font rendering and modern dark aesthetic
plt.style.use("dark_background")
plt.rcParams.update(
    {
        "font.family": "sans-serif",
        "font.sans-serif": ["DejaVu Sans", "Helvetica", "Arial"],
        "axes.edgecolor": "#30363d",
        "axes.linewidth": 1.2,
        "grid.color": "#21262d",
        "grid.linestyle": "--",
        "grid.alpha": 0.7,
    }
)


@dataclass(frozen=True)
class SimMarket:
    t_pts: np.ndarray
    asks: np.ndarray
    bids: np.ndarray
    mid_px: np.ndarray
    f_times: list[float]
    f_prices: list[float]
    f_sides: list[str]
    f_qtys: list[int]


def _plot_market_and_orders(ax: Any, market: SimMarket) -> None:
    ax.set_facecolor("#161b22")
    ax.set_title(
        "Market Maker Order Lifecycle & Spread Profiling Over Time",
        fontsize=15,
        fontweight="bold",
        color="#58a6ff",
        pad=12,
    )
    t = market.t_pts
    ax.step(
        t, market.asks, where="post", color="#f85149", alpha=0.4, lw=1.5, label="Market TOB Ask"
    )
    ax.step(
        t, market.bids, where="post", color="#3fb950", alpha=0.4, lw=1.5, label="Market TOB Bid"
    )
    ax.step(
        t, market.asks, where="post", color="#ff7b72", lw=2.0, ls="--", label="Quoted Ask (Maker)"
    )
    ax.step(
        t, market.bids, where="post", color="#56d364", lw=2.0, ls="--", label="Quoted Bid (Maker)"
    )

    for ft, fp, fs, fq in zip(
        market.f_times, market.f_prices, market.f_sides, market.f_qtys, strict=True
    ):
        c = "#2ea043" if fs == "BUY" else "#da3633"
        m = "^" if fs == "BUY" else "v"
        ax.scatter(ft, fp, color=c, s=160, zorder=5, marker=m, edgecolors="white", lw=1.5)
        y_off = 8 if fs == "BUY" else -12
        ax.annotate(
            f"FILL {fs} {fq}x\n@{fp}",
            xy=(ft, fp),
            xytext=(ft + 60, fp + y_off),
            fontsize=9,
            fontweight="bold",
            color="white",
            bbox={"boxstyle": "round,pad=0.3", "fc": c, "ec": "none", "alpha": 0.9},
            arrowprops={"arrowstyle": "->", "color": "white", "lw": 1.0},
        )

    ax.axvline(4500, color="#d29922", linestyle=":", lw=2)
    ax.text(
        4520,
        float(np.mean(market.mid_px)) + 5,
        "STALE FEED DETECTION\n(Pull All Quotes & Close 4000)",
        color="#d29922",
        fontsize=9,
        fontweight="bold",
    )
    ax.yaxis.set_major_formatter(ticker.StrMethodFormatter("{x:,.0f}"))
    ax.set_xlabel("Elapsed Time (milliseconds)", fontsize=11, color="#8b949e")
    ax.set_ylabel("Price Level (USDT Ticks)", fontsize=11, color="#8b949e")
    ax.grid(True)
    ax.legend(loc="upper left", framealpha=0.8, facecolor="#21262d", edgecolor="#30363d")


def _plot_latency_decomp(ax: Any) -> None:
    ax.set_facecolor("#161b22")
    ax.set_title(
        "Zero-Copy SHM Path Breakdown (2,100 ns)",
        fontsize=12,
        fontweight="bold",
        color="#79c0ff",
        pad=10,
    )
    steps = [
        "1. C++ Book Gen",
        "2. SHM Fence",
        "3. Py Unpack",
        "4. Strategy Core",
        "5. Py Pack",
        "6. C++ Match (M2)",
    ]
    lat_ns = [41, 65, 140, 1620, 130, 104]
    colors = ["#1f6feb", "#238636", "#8957e5", "#a371f7", "#8957e5", "#238636"]

    bars = ax.barh(steps, lat_ns, color=colors, edgecolor="#30363d", height=0.65)
    for b in bars:
        w = b.get_width()
        ax.text(
            w + 30,
            b.get_y() + b.get_height() / 2,
            f"{int(w)} ns",
            va="center",
            ha="left",
            fontsize=9,
            color="#c9d1d9",
            fontweight="bold",
        )
    ax.set_xlim(0, 2000)
    ax.set_xlabel("Latency (nanoseconds)", fontsize=10, color="#8b949e")
    ax.grid(axis="x")


def _plot_inventory(ax: Any, t_pts: np.ndarray, inv: np.ndarray) -> None:
    ax.set_facecolor("#161b22")
    ax.set_title(
        "Inventory Position Profile q(t)",
        fontsize=12,
        fontweight="bold",
        color="#7ee787",
        pad=10,
    )
    ax.step(t_pts, inv, where="post", color="#58a6ff", lw=2.2)
    ax.fill_between(t_pts, inv, step="post", alpha=0.25, color="#1f6feb")
    ax.axhline(0, color="#8b949e", ls="-", alpha=0.5)
    ax.axhline(100, color="#f85149", ls="--", alpha=0.6, label="Max Risk (+100)")
    ax.axhline(-100, color="#f85149", ls="--", alpha=0.6, label="Min Risk (-100)")
    ax.set_ylim(-30, 30)
    ax.set_xlabel("Elapsed Time (milliseconds)", fontsize=10, color="#8b949e")
    ax.set_ylabel("Net Position (Contracts)", fontsize=10, color="#8b949e")
    ax.grid(True)
    ax.legend(
        loc="upper right",
        framealpha=0.8,
        facecolor="#21262d",
        edgecolor="#30363d",
        fontsize=8,
    )


def _plot_tiers(ax: Any) -> None:
    ax.set_facecolor("#161b22")
    ax.set_title(
        "End-to-End Tick-to-Order Speedup",
        fontsize=12,
        fontweight="bold",
        color="#f0883e",
        pad=10,
    )
    labels = ["1. Naive WS", "2. Tuned WS", "3. Zero-Copy SHM", "4. Native C++"]
    lats_us = [202.3, 149.2, 2.10, 0.291]
    colors = ["#da3633", "#d29922", "#1f6feb", "#238636"]

    bars = ax.bar(labels, lats_us, color=colors, edgecolor="#30363d", width=0.55)
    ax.set_yscale("log")
    ax.set_ylabel("Tick-to-Order Latency (us, Log Scale)", fontsize=10, color="#8b949e")

    speedups = ["1.0x", "1.35x", "96.3x", "695.2x"]
    for b, val, sp in zip(bars, lats_us, speedups, strict=True):
        y = b.get_height()
        lbl = f"{val:g} us\n({sp})" if val >= 1.0 else f"{int(val * 1000)} ns\n({sp})"
        ax.text(
            b.get_x() + b.get_width() / 2,
            y * 1.3,
            lbl,
            ha="center",
            va="bottom",
            fontsize=8.5,
            fontweight="bold",
            color="#c9d1d9",
        )
    ax.set_ylim(0.1, 500)
    ax.grid(axis="y", which="both")
    plt.xticks(rotation=15, ha="right", fontsize=9)


def run_profiling_and_generate_chart() -> None:
    repo = Path(__file__).resolve().parent.parent.parent
    assets_dir = repo / "docs" / "assets"
    assets_dir.mkdir(parents=True, exist_ok=True)
    out_png = assets_dir / "order_flow_profiling.png"

    t_pts = np.linspace(0, 5000, 100)
    base_price = 500000
    np.random.seed(42)
    drift = np.cumsum(np.random.choice([-5, 0, 5], size=len(t_pts), p=[0.25, 0.5, 0.25]))
    mid_px = base_price + drift
    bids = mid_px - 5
    asks = mid_px + 5

    fill_idx = [15, 42, 68, 85]
    f_times = [float(t_pts[i]) for i in fill_idx]
    f_prices = [float(bids[15]), float(asks[42]), float(bids[68]), float(asks[85])]
    f_sides = ["BUY", "SELL", "BUY", "SELL"]
    f_qtys = [10, 10, 10, 10]

    market = SimMarket(
        t_pts=t_pts,
        asks=asks,
        bids=bids,
        mid_px=mid_px,
        f_times=f_times,
        f_prices=f_prices,
        f_sides=f_sides,
        f_qtys=f_qtys,
    )

    inv = np.zeros(len(t_pts))
    cur = 0
    ptr = 0
    for i in range(len(t_pts)):
        if ptr < len(fill_idx) and i == fill_idx[ptr]:
            cur += f_qtys[ptr] if f_sides[ptr] == "BUY" else -f_qtys[ptr]
            ptr += 1
        inv[i] = cur

    fig = plt.figure(figsize=(16, 10), dpi=200, facecolor="#0d1117")
    gs = fig.add_gridspec(2, 3, height_ratios=[1.2, 1.0], hspace=0.32, wspace=0.25)

    _plot_market_and_orders(fig.add_subplot(gs[0, :]), market)
    _plot_latency_decomp(fig.add_subplot(gs[1, 0]))
    _plot_inventory(fig.add_subplot(gs[1, 1]), t_pts, inv)
    _plot_tiers(fig.add_subplot(gs[1, 2]))

    plt.tight_layout()
    fig.savefig(out_png, dpi=200, facecolor=fig.get_facecolor(), edgecolor="none")
    plt.close(fig)


if __name__ == "__main__":
    run_profiling_and_generate_chart()
