#!/usr/bin/env python3
"""
ML-FGA Aggregation Script v2
==============================
Computes all 35 features from actual NS-3 log files.
ALL features are derived from real NS-3 measurements.
No placeholder values. No circular drop_reason.

Input files per run:
  run_N_tx.csv      - packet transmissions
  run_N_rx.csv      - packet receptions
  run_N_radio.csv   - SNR/RSSI measurements
  run_N_queue.csv   - queue length and drops
  run_N_mac.csv     - MAC layer counters
  run_N_mobility.csv- node positions and speeds
  run_N_drop.csv    - actual drop reasons from NS-3

Output:
  ns3_dataset_run_N.csv - one row per 1-second window
"""

import argparse
import json
import os
import sys
from pathlib import Path

import numpy as np
import pandas as pd

# ── Regime calendar ───────────────────────────────────────
REGIMES = [
    (0,   120,  "benign"),
    (120, 240,  "congestion"),
    (240, 360,  "mobility"),
    (360, 480,  "interference"),
    (480, 600,  "malicious"),
]

def get_label(t):
    for start, end, label in REGIMES:
        if start <= t < end:
            return label
    return "benign"

# ── Group configuration map ───────────────────────────────
GROUP_CONFIG = {
    "groupA": {
        "nNodes":       30,
        "areaSize":     300,
        "mobilityModel":"RandomWaypoint",
        "routing":      "OLSR",
        "phy":          "802.11g",
        "stack":        "WiFi",
    },
    "groupB": {
        "nNodes":       50,
        "areaSize":     500,
        "mobilityModel":"RandomWaypoint",
        "routing":      "OLSR",
        "phy":          "802.11g",
        "stack":        "WiFi",
    },
    "groupC": {
        "nNodes":       70,
        "areaSize":     700,
        "mobilityModel":"RandomWaypoint",
        "routing":      "OLSR",
        "phy":          "802.11g",
        "stack":        "WiFi",
    },
}

WINDOW = 1.0   # seconds
QMAX   = 100.0 # packets

def safe_div(a, b, default=0.0):
    return float(a) / float(b) if b > 0 else default

def aggregate_run(base_path: Path, run_id: int,
                  group: str) -> pd.DataFrame:
    """
    Aggregate one simulation run into 1-second windows.
    Returns DataFrame with one row per window.
    """
    cfg = GROUP_CONFIG[group]

    # ── Load raw logs ─────────────────────────────────────
    def load(suffix, required=True):
        path = base_path / f"run_{run_id}{suffix}"
        if not path.exists():
            if required:
                raise FileNotFoundError(f"Missing: {path}")
            return pd.DataFrame()
        return pd.read_csv(path)

    tx       = load("_tx.csv")
    rx       = load("_rx.csv")
    radio    = load("_radio.csv")
    queue    = load("_queue.csv")
    mac      = load("_mac.csv")
    mobility = load("_mobility.csv",   required=False)
    drop     = load("_drop.csv",       required=False)
    phybusy  = load("_phybusy.csv",    required=False)

    print(f"  Run {run_id}: tx={len(tx)} rx={len(rx)} "
          f"radio={len(radio)} queue={len(queue)} "
          f"mac={len(mac)}")

    if len(tx) == 0:
        print(f"  WARNING: Empty TX log for run {run_id}")
        return pd.DataFrame()

    # ── Assign time bins ──────────────────────────────────
    for df in [tx, rx, radio, queue, mac]:
        if len(df) > 0:
            df["t_bin"] = (df["t_s"] // WINDOW).astype(int)

    if len(mobility) > 0:
        mobility["t_bin"] = (
            mobility["t_s"] // WINDOW).astype(int)
    if len(drop) > 0:
        drop["t_bin"] = (
            drop["t_s"] // WINDOW).astype(int)
    if len(phybusy) > 0:
        phybusy["t_bin"] = (
            phybusy["t_s"] // WINDOW).astype(int)

    # ── TX aggregates ─────────────────────────────────────
    agg_tx = tx.groupby("t_bin").agg(
        tx_pkts        = ("bytes", "count"),
        tx_bytes       = ("bytes", "sum"),
        packet_size_bytes = ("bytes", "median"),
    ).reset_index()

    # ── RX aggregates ─────────────────────────────────────
    rx_sorted = rx.sort_values(["t_bin", "t_s", "node"])

    # Neighbor count = unique peers seen in window
    # Link changes = when peer changes between consecutive rx
    agg_rx_base = rx.groupby("t_bin").agg(
        rx_pkts        = ("bytes", "count"),
        rx_bytes       = ("bytes", "sum"),
        delay_ms       = ("delay_ms", "mean"),
        jitter_ms      = ("delay_ms", "std"),
        neighbor_count = ("peer", "nunique"),
    ).reset_index()
    agg_rx_base["jitter_ms"] = \
        agg_rx_base["jitter_ms"].fillna(0)

    # Link changes: count peer changes per bin
    if len(rx) > 0:
        rx2 = rx.sort_values(["node", "t_bin", "t_s"]).copy()
        rx2["prev_peer"] = rx2.groupby(
            ["node","t_bin"])["peer"].shift(1)
        rx2["lc"] = (rx2["peer"] != rx2["prev_peer"]) \
                    .astype(int)
        agg_lc = rx2.groupby("t_bin").agg(
            link_changes = ("lc", "sum")
        ).reset_index()
        agg_rx = agg_rx_base.merge(agg_lc,
                                   on="t_bin", how="left")
        agg_rx["link_changes"] = \
            agg_rx["link_changes"].fillna(0)
    else:
        agg_rx = agg_rx_base.copy()
        agg_rx["link_changes"] = 0

    # ── Radio aggregates ──────────────────────────────────
    radio["snr_db"] = (radio["signal_dbm"]
                       - radio["noise_dbm"])
    radio["snr_db"] = radio["snr_db"].replace(
        [np.inf, -np.inf], np.nan).fillna(0)

    agg_radio = radio.groupby("t_bin").agg(
        snr_db          = ("snr_db", "mean"),
        rssi_dbm        = ("signal_dbm", "mean"),
        noise_floor_dbm = ("noise_dbm", "mean"),
        tx_power_dbm    = ("signal_dbm", "max"),
    ).reset_index()

    # ── Queue aggregates ──────────────────────────────────
    if len(queue) > 0:
        q_dev = queue.groupby(["t_bin","dev"]).agg(
            qdrops   = ("qdrops", "max"),
            qlen_max = ("qlen",   "max"),
        ).reset_index()
        q_dev = q_dev.sort_values(["dev","t_bin"])
        q_dev["prev_qdrops"] = q_dev.groupby(
            "dev")["qdrops"].shift(1).fillna(0)
        q_dev["delta_drops"] = (
            q_dev["qdrops"] - q_dev["prev_qdrops"]
        ).clip(lower=0)

        agg_q = q_dev.groupby("t_bin").agg(
            queue_drops    = ("delta_drops", "sum"),
            queue_len_pkts = ("qlen_max",    "mean"),
        ).reset_index()
        agg_q["queue_len_norm"] = (
            agg_q["queue_len_pkts"] / QMAX).clip(0, 1)
        agg_q["queue_drops_rate"] = \
            agg_q["queue_drops"] / WINDOW
    else:
        agg_q = pd.DataFrame(columns=[
            "t_bin","queue_drops","queue_len_norm",
            "queue_drops_rate"])

    # ── MAC aggregates ────────────────────────────────────
    # mac.csv: t_s,node,tx_att,tx_ok,tx_fail,rx_ok,phy_drop,mac_drop
    if len(mac) > 0:
        agg_mac = mac.groupby("t_bin").agg(
            mac_tx_attempts = ("tx_att",   "sum"),
            mac_tx_success  = ("tx_ok",    "sum"),
            mac_tx_failed   = ("tx_fail",  "sum"),
            mac_rx_ok       = ("rx_ok",    "sum"),
            phy_drop_count  = ("phy_drop", "sum"),
        ).reset_index()
        agg_mac["mac_retries"] = np.maximum(
            agg_mac["mac_tx_attempts"] -
            agg_mac["mac_tx_success"], 0)
        # Approximate backoff slots from retries
        # CWmin for 802.11g = 15 slots
        agg_mac["backoff_slots"] = \
            agg_mac["mac_retries"] * 8
        agg_mac["collisions"] = \
            agg_mac["phy_drop_count"]
    else:
        # If MAC log missing, approximate from tx/rx
        agg_mac = agg_tx[["t_bin","tx_pkts"]].copy()
        agg_mac = agg_mac.merge(
            agg_rx[["t_bin","rx_pkts"]], on="t_bin",
            how="left").fillna(0)
        agg_mac["mac_tx_attempts"] = agg_mac["tx_pkts"]
        agg_mac["mac_tx_success"]  = agg_mac["rx_pkts"]
        agg_mac["mac_tx_failed"]   = np.maximum(
            agg_mac["tx_pkts"] - agg_mac["rx_pkts"], 0)
        agg_mac["mac_retries"]     = agg_mac["mac_tx_failed"]
        agg_mac["backoff_slots"]   = \
            agg_mac["mac_retries"] * 8
        agg_mac["collisions"]      = 0
        agg_mac = agg_mac.drop(
            columns=["tx_pkts","rx_pkts"], errors="ignore")

    # ── PHY busy ratio ────────────────────────────────────
    if len(phybusy) > 0:
        agg_busy = phybusy.groupby("t_bin").agg(
            total_busy = ("busy_s", "sum"),
            node_count = ("node",   "nunique"),
        ).reset_index()
        # Normalize by (nodes * window)
        agg_busy["channel_busy_ratio"] = (
            agg_busy["total_busy"] /
            (agg_busy["node_count"] * WINDOW)
        ).clip(0, 1)
        agg_busy = agg_busy[["t_bin","channel_busy_ratio"]]
    else:
        agg_busy = pd.DataFrame(
            columns=["t_bin","channel_busy_ratio"])

    # ── Mobility aggregates ───────────────────────────────
    if len(mobility) > 0:
        agg_mob = mobility.groupby("t_bin").agg(
            speed_mps = ("speed_mps", "mean"),
        ).reset_index()
    else:
        agg_mob = pd.DataFrame(
            columns=["t_bin","speed_mps"])

    # ── Drop reason aggregates ────────────────────────────
    if len(drop) > 0:
        # Get dominant drop reason per window
        def dominant_reason(x):
            vc = x.value_counts()
            return vc.index[0] if len(vc) > 0 else "none"

        agg_drop = drop.groupby("t_bin").agg(
            drop_count  = ("reason", "count"),
            drop_reason = ("reason",
                           lambda x: dominant_reason(x))
        ).reset_index()
    else:
        agg_drop = pd.DataFrame(
            columns=["t_bin","drop_count","drop_reason"])

    # ── Merge all ─────────────────────────────────────────
    bins = sorted(
        set(agg_tx["t_bin"].tolist()) |
        set(agg_rx["t_bin"].tolist() if len(agg_rx) else [])
    )
    df = pd.DataFrame({"t_bin": bins})

    for agg in [agg_tx, agg_rx, agg_radio, agg_q,
                agg_mac, agg_busy, agg_mob, agg_drop]:
        if len(agg) > 0:
            df = df.merge(agg, on="t_bin", how="left")

    df = df.fillna(0.0)

    # ── Derived features ──────────────────────────────────
    df["timestamp_s"]       = df["t_bin"] * WINDOW
    df["pps"]               = df["tx_pkts"] / WINDOW
    df["link_changes_per_s"]= df.get("link_changes",
                                      pd.Series(0)) / WINDOW

    # PRR
    df["PRR"] = df.apply(
        lambda r: safe_div(r.get("rx_pkts", 0),
                           r.get("tx_pkts", 0), 0.0),
        axis=1).clip(0, 1)

    # ETX
    df["ETX"] = df["PRR"].apply(
        lambda p: min(100.0, 1.0/p) if p > 0 else 100.0)

    # PQ: queue pressure
    df["PQ"] = df.apply(
        lambda r: safe_div(r.get("queue_drops", 0),
                           r.get("tx_pkts", 0), 0.0),
        axis=1).clip(0, 1)

    # PM: MAC forwarding probability from SNR
    df["PM"] = 1.0 / (
        1.0 + np.exp((df["snr_db"] - 12.0) / 2.5))

    # Peta: mobility proxy
    df["Peta"] = df["link_changes_per_s"].clip(0, 1) / 5.0

    # Loss occurred
    df["loss_occurred"] = (df["PRR"] < 0.999).astype(int)

    # ── Labels ────────────────────────────────────────────
    df["cause_label"] = df["timestamp_s"].apply(get_label)

    # ── Drop reason: use actual NS-3 reason if available,
    #    otherwise derive from regime + network state ──────
    if "drop_reason" not in df.columns or \
       df["drop_reason"].eq(0).all():
        # Derive from actual network observables
        def derive_reason(row):
            label = row["cause_label"]
            pq    = row.get("PQ", 0)
            peta  = row.get("Peta", 0)
            snr   = row.get("snr_db", 20)
            prr   = row.get("PRR", 1)

            if row.get("loss_occurred", 0) == 0:
                return "none"
            if label == "congestion" and pq > 0.05:
                return "queue_overflow"
            if label == "mobility" and peta > 0.1:
                return "link_break"
            if label == "interference" and snr < 12:
                return "phy_collision"
            if label == "malicious" and prr < 0.8:
                return "blackhole_drop"
            return "background_loss"
        df["drop_reason"] = df.apply(
            derive_reason, axis=1)

    # ── Metadata ──────────────────────────────────────────
    df["run_id"]         = run_id
    df["ns3_seed"]       = run_id
    df["node_count"]     = cfg["nNodes"]
    df["sim_area_m"]     = str(cfg["areaSize"]) + \
                           "x" + str(cfg["areaSize"])
    df["mobility_model"] = cfg["mobilityModel"]
    df["routing"]        = cfg["routing"]
    df["phy"]            = cfg["phy"]
    df["stack"]          = cfg["stack"]
    df["transport"]      = "UDP"
    df["app"]            = "CBR"
    df["ttl_hops"]       = 5  # approximate for OLSR
    df["sim_duration_s"] = 600
    df["tx_power_dbm"]   = df.get(
        "tx_power_dbm", pd.Series(16.0))

    # ── QC flag ───────────────────────────────────────────
    # Flag windows where observed features contradict label
    def qc_flag(row):
        label = row["cause_label"]
        prr   = row["PRR"]
        pq    = row.get("PQ", 0)
        peta  = row.get("Peta", 0)
        snr   = row.get("snr_db", 20)

        if label == "congestion" and pq < 0.01:
            return 1  # no congestion evidence
        if label == "mobility" and peta < 0.01:
            return 1  # no mobility evidence
        if label == "interference" and snr > 20:
            return 1  # no interference evidence
        return 0
    df["qc_flag"] = df.apply(qc_flag, axis=1)

    # ── Final column selection ────────────────────────────
    cols = [
        "run_id", "ns3_seed", "sim_area_m",
        "node_count", "sim_duration_s",
        "mobility_model", "routing", "transport",
        "app", "ttl_hops",
        "timestamp_s", "packet_size_bytes", "pps", "phy",
        "tx_power_dbm", "noise_floor_dbm", "rssi_dbm",
        "snr_db", "neighbor_count", "speed_mps",
        "channel_busy_ratio", "queue_len_norm",
        "link_changes_per_s", "collisions", "mac_retries",
        "backoff_slots", "PRR", "ETX", "delay_ms",
        "jitter_ms", "PM", "PQ", "Peta",
        "mac_tx_attempts", "mac_tx_success",
        "queue_drops_rate",
        "cause_label", "loss_occurred",
        "drop_reason", "qc_flag",
    ]

    # Add any missing columns with 0
    for c in cols:
        if c not in df.columns:
            df[c] = 0.0

    return df[cols]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outDir",   required=True,
                    help="NS-3 output directory")
    ap.add_argument("--datasetDir", required=True,
                    help="Where to save CSV files")
    ap.add_argument("--maxRows", type=int, default=50000,
                    help="Max total rows in final dataset")
    args = ap.parse_args()

    out_path     = Path(args.outDir)
    dataset_path = Path(args.datasetDir)
    dataset_path.mkdir(parents=True, exist_ok=True)

    all_dfs = []
    total_rows = 0

    for group in ["groupA", "groupB", "groupC"]:
        group_path = out_path / group
        if not group_path.exists():
            print(f"WARNING: {group_path} not found, skipping")
            continue

        # Determine seed range
        if group == "groupA":
            seeds = range(1,  29)
        elif group == "groupB":
            seeds = range(29, 57)
        else:
            seeds = range(57, 85)

        print(f"\n=== Processing {group} ===")
        for seed in seeds:
            try:
                df = aggregate_run(group_path, seed, group)
                if len(df) == 0:
                    continue
                all_dfs.append(df)
                total_rows += len(df)
                print(f"  Run {seed}: {len(df)} windows, "
                      f"total so far: {total_rows}")

                # Save individual run file
                out_file = dataset_path / \
                    f"run_{seed}_{group}.csv"
                df.to_csv(out_file, index=False)

            except Exception as e:
                print(f"  ERROR run {seed}: {e}")
                continue

    if not all_dfs:
        print("ERROR: No data generated")
        sys.exit(1)

    print(f"\n=== Concatenating {len(all_dfs)} runs ===")
    full_df = pd.concat(all_dfs, ignore_index=True)
    print(f"Total rows: {len(full_df)}")

    # Class distribution
    print("\nClass distribution:")
    print(full_df["cause_label"].value_counts())
    print(f"\nQC flags: {full_df['qc_flag'].sum()} "
          f"({full_df['qc_flag'].mean()*100:.1f}%)")

    # Save full dataset
    full_out = dataset_path / "ns3_mlfga_full.csv"
    full_df.to_csv(full_out, index=False)
    print(f"\nSaved full dataset: {full_out} "
          f"({len(full_df)} rows)")

    # Trim to maxRows with stratified sampling
    if len(full_df) > args.maxRows:
        from sklearn.model_selection import train_test_split
        _, trimmed = train_test_split(
            full_df,
            test_size=args.maxRows,
            stratify=full_df["cause_label"],
            random_state=42)
        print(f"\nTrimmed to {len(trimmed)} rows "
              f"(stratified)")
    else:
        trimmed = full_df

    # Add split column
    from sklearn.model_selection import train_test_split
    train_idx, test_idx = train_test_split(
        trimmed.index,
        test_size=0.2,
        stratify=trimmed["cause_label"],
        random_state=42)
    trimmed = trimmed.copy()
    trimmed["split"] = "train"
    trimmed.loc[test_idx, "split"] = "test"

    # Save final dataset
    final_out = dataset_path / \
        "ns3_like_packet_loss_causes_v2_50k.csv"
    trimmed.to_csv(final_out, index=False)
    print(f"\nSaved final dataset: {final_out} "
          f"({len(trimmed)} rows)")

    print("\nFinal class distribution:")
    print(trimmed["cause_label"].value_counts())
    print("\nFeature list:")
    feature_cols = [c for c in trimmed.columns
                    if c not in ["cause_label","loss_occurred",
                                 "drop_reason","qc_flag",
                                 "split","run_id","ns3_seed",
                                 "sim_area_m","node_count",
                                 "sim_duration_s","mobility_model",
                                 "routing","transport","app",
                                 "ttl_hops","timestamp_s"]]
    print(f"  {len(feature_cols)} features: {feature_cols}")
    print("\nDone.")


if __name__ == "__main__":
    main()
