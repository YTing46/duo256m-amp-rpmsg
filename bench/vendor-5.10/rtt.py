#!/usr/bin/env python3
"""
rtt_decomp_analyze.py - 分析 rpmsg_rtt_decomp4 產生的 rtt_decomp4.csv

用法:
    python3 rtt_decomp_analyze.py [rtt_decomp4.csv]

輸出:
    - 終端機:各段 min/p50/p90/p99/max/mean 統計表(中文)
    - rtt_stacked.png    : RTT 五段拆解堆疊長條圖(p50,佔比標註)— 報告主圖
    - rtt_tail.png       : 各段 p50/p99/max 對照(log 軸)— 長尾來源一目了然
    - rtt_decomp_hist.png: 各段分布直方圖
圖內標籤用英文(避免 matplotlib 缺中文字型變方框);報告圖說可自行配中文。
CSV 欄位: iter,rtt,A,workqueue,callback,rxC,turnaround,transport
"""
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

csv = sys.argv[1] if len(sys.argv) > 1 else "rtt_decomp4.csv"
data = np.genfromtxt(csv, delimiter=",", names=True)

# (csv欄位, 終端機中文標籤, 圖內英文標籤) — 堆疊順序按時間先後
segs = [
    ("A",          "A 大核送出",      "big-core send (A)"),
    ("transport",  "transport(兩段)", "transport (both ways)"),
    ("turnaround", "小核 turnaround", "small-core turnaround"),
    ("workqueue",  "workqueue 延遲",  "workqueue latency"),
    ("callback",   "callback+喚醒",   "callback + wakeup"),
]
colors = ["#4C72B0", "#55A868", "#C44E52", "#8172B2", "#CCB974"]

# ---- 統計表(終端機,中文) ----
hdr = f"{'segment':<18} {'min':>8} {'p50':>8} {'p90':>8} {'p99':>8} {'max':>9} {'mean':>9}  (ns)"
print(hdr); print("-" * len(hdr))
stats = {}
for key, zh, _ in segs + [("rtt", "RTT 全部", "RTT")]:
    v = data[key]
    stats[key] = dict(p50=np.percentile(v, 50), p99=np.percentile(v, 99), mx=v.max())
    print(f"{zh:<18} {v.min():>8.0f} {np.percentile(v,50):>8.0f} {np.percentile(v,90):>8.0f} "
          f"{np.percentile(v,99):>8.0f} {v.max():>9.0f} {v.mean():>9.0f}")

rtt_p50 = stats["rtt"]["p50"]
seg_p50 = [stats[k]["p50"] for k, _, _ in segs]
print(f"\n五段 p50 加總 = {sum(seg_p50)/1000:.1f} µs   vs   RTT p50 = {rtt_p50/1000:.1f} µs"
      f"   (差 {abs(sum(seg_p50)-rtt_p50)/1000:.1f} µs → 分解自洽程度)")

# ---- 圖 1:堆疊長條(報告主圖) ----
fig, ax = plt.subplots(figsize=(5.5, 6.5))
bottom = 0.0
for (key, _, en), c in zip(segs, colors):
    v = stats[key]["p50"] / 1000.0  # µs
    ax.bar(["RTT p50"], [v], bottom=bottom, color=c, width=0.45,
           label=f"{en}  {v:.1f} µs ({v*1000/rtt_p50*100:.0f}%)")
    ax.text(0, bottom + v/2, f"{v:.1f}", ha="center", va="center", fontsize=10)
    bottom += v
ax.set_ylabel("latency (µs)")
ax.set_title(f"RPMsg RTT decomposition (p50 = {rtt_p50/1000:.1f} µs)")
ax.legend(loc="upper left", bbox_to_anchor=(1.02, 1.0), fontsize=9, frameon=False)
plt.savefig("rtt_stacked.png", dpi=130, bbox_inches="tight")

# ---- 圖 2:p50 vs p99 vs max(長尾來源) ----
fig, ax = plt.subplots(figsize=(8.5, 4.5))
x = np.arange(len(segs))
w = 0.27
ax.bar(x - w, [stats[k]["p50"]/1000 for k, _, _ in segs], w, label="p50")
ax.bar(x,     [stats[k]["p99"]/1000 for k, _, _ in segs], w, label="p99")
ax.bar(x + w, [stats[k]["mx"]/1000  for k, _, _ in segs], w, label="max")
ax.set_xticks(x); ax.set_xticklabels([en for _, _, en in segs], fontsize=8)
ax.set_yscale("log")
ax.set_ylabel("latency (µs, log scale)")
ax.set_title("Tail latency by segment — where the spikes live")
ax.legend()
plt.savefig("rtt_tail.png", dpi=130, bbox_inches="tight")

# ---- 圖 3:各段分布 ----
fig, axes = plt.subplots(1, len(segs), figsize=(16, 3))
for axx, (key, _, en), c in zip(axes, segs, colors):
    v = data[key]
    cut = np.percentile(v, 99)
    axx.hist(v[v <= cut]/1000, bins=60, color=c)
    axx.set_title(en, fontsize=9)
    axx.set_xlabel("µs", fontsize=8)
plt.tight_layout()
plt.savefig("rtt_decomp_hist.png", dpi=130, bbox_inches="tight")

print("\n已輸出 rtt_stacked.png / rtt_tail.png / rtt_decomp_hist.png")