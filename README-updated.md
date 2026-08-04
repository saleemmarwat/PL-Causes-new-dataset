# PL-CausesNS3-50k

**ML-FGA: A Machine Learning Framework for Fine-Grained Analysis of Packet Loss Causes in MANETs and IoT Networks**

[![IEEE DataPort](https://img.shields.io/badge/Dataset-IEEE%20DataPort-blue)](https://ieee-dataport.org/documents/pl-causesns3-50k)
[![DOI](https://img.shields.io/badge/DOI-10.21227%2F86dj--xd45-orange)](https://doi.org/10.21227/86dj-xd45)
[![IEEE Access](https://img.shields.io/badge/Journal-IEEE%20Access-green)](https://ieeeaccess.ieee.org/)

---

## Overview

This repository contains the complete reproducibility package for the ML-FGA paper. The dataset comprises **50,000 records** spanning five packet loss cause categories, generated from **84 independent NS-3.45 simulation runs** across three node-density and area configurations under IEEE 802.11g WiFi adhoc conditions with OLSR routing.

All five cause-specific network regimes are implemented **directly in the NS-3.45 simulator** via scheduled callbacks — not as post-hoc label assignments. This ensures that ground-truth labels reflect genuine changes in network conditions.

---

## Repository Structure

```
PL-Causes-ns3-dataset/
│
├── plc_scenario_final.cc                        # NS-3.45 simulation script (v2)
├── run_84_aws.sh                                # Shell script to run all 84 simulations
├── aggregate_v2.py                              # Feature aggregation pipeline
├── calendar_600_5regimes.json                   # Temporal regime calendar
├── ns3_like_packet_loss_causes_v2_50k.csv       # Final labeled dataset (50,000 records)
├── packet_loss_classification_after_review.ipynb # Training and evaluation notebook
└── README.md
```

---

## Requirements

### NS-3 Simulation
```
NS-3.45 (https://www.nsnam.org/releases/ns-3-45/)
  Modules: core, network, mobility, internet,
           wifi, olsr, applications, traffic-control
C++ compiler: g++ >= 9.0
Ubuntu 20.04 or 22.04 recommended
```

### Python Pipeline
```
Python >= 3.8
pandas >= 1.3
numpy >= 1.21
scikit-learn >= 1.0
xgboost >= 1.6
imbalanced-learn >= 0.9
shap >= 0.41
matplotlib >= 3.4
seaborn >= 0.12
```

Install all Python dependencies:
```bash
pip install pandas numpy scikit-learn xgboost \
    imbalanced-learn shap matplotlib seaborn
```

---

## Step-by-Step Reproduction

### Step 1 — Install NS-3.45

```bash
wget https://www.nsnam.org/releases/ns-allinone-3.45.tar.bz2
tar -xjf ns-allinone-3.45.tar.bz2
cd ns-allinone-3.45/ns-3.45

./ns3 configure \
    --enable-examples \
    --disable-werror \
    --disable-python

./ns3 build 2>&1 | tail -5
```

### Step 2 — Copy Simulation Script

```bash
cp plc_scenario_final.cc \
   ns-allinone-3.45/ns-3.45/scratch/

cd ns-allinone-3.45/ns-3.45
./ns3 build 2>&1 | grep -E "error:|Finished"
```

### Step 3 — Run All 84 Simulation Runs

```bash
chmod +x run_84_aws.sh

# Run with nohup to survive disconnection
nohup bash run_84_aws.sh > run_84_log.txt 2>&1 &
echo "PID: $!"

# Monitor progress
tail -f run_84_log.txt
```

The script runs 4 simulations in parallel across three configuration groups:

| Group | Seeds | Nodes | Area | Duration |
|---|---|---|---|---|
| A | 1--28 | 20 | 150×150 m | ~2 hours |
| B | 29--56 | 30 | 200×200 m | ~2 hours |
| C | 57--84 | 40 | 250×250 m | ~2 hours |

Each run produces **eight output log files**:
```
run_N_tx.csv       — packet transmissions
run_N_rx.csv       — receptions with per-packet delay
run_N_radio.csv    — SNR/RSSI from MonitorSnifferRx
run_N_queue.csv    — FQ-CoDel queue length and drops
run_N_mac.csv      — MAC counters (MacTx, MacTxDrop)
run_N_mobility.csv — node positions and speeds
run_N_drop.csv     — drop events with reason
run_N_phybusy.csv  — PHY busy time
```

### Step 4 — Aggregate Features

```bash
mkdir -p ~/ns3-dataset-v2

python3 aggregate_v2.py \
    --outDir ~/ns3-output-v2 \
    --datasetDir ~/ns3-dataset-v2 \
    --maxRows 50000
```

This script:
- Reads raw NS-3.45 logs for each run
- Computes 15 genuine per-window network-layer features
- Assigns ground-truth labels using the temporal regime calendar
- Outputs the final 50,000-record balanced dataset

### Step 5 — Train and Evaluate Models

Open and run the Jupyter notebook:

```bash
jupyter notebook packet_loss_classification_after_review.ipynb
```

Or upload to Google Colab and run all cells. The notebook reproduces:
- XGBoost, Random Forest, SVM, MLP training
- Per-class performance analysis
- McNemar's test (all pairwise comparisons)
- 5-fold cross-validation (83.94% ± 0.35%)
- SMOTE and cost-sensitive experiments
- SHAP explainability analysis (10,000-sample)
- All figures from Section VI of the paper

---

## Dataset Description

| Property | Value |
|---|---|
| Total records | 50,000 |
| Features | 15 genuine network-layer observables |
| Classes | 5 (benign, congestion, mobility, interference, malicious) |
| Class balance | Perfectly balanced — 10,000 per class |
| Train / Test split | 80% / 20% (stratified) |
| Simulation runs | 84 (seeds 1--84) |
| Simulation duration | 620 seconds per run |
| PHY / MAC | IEEE 802.11g, WiFi adhoc |
| Routing | OLSR (proactive) |
| Queue | FQ-CoDel |
| Mobility | RandomWaypoint |

### Class Distribution

| Class | Records | % | Train | Test |
|---|---|---|---|---|
| benign | 10,000 | 20.0 | 8,000 | 2,000 |
| congestion | 10,000 | 20.0 | 8,000 | 2,000 |
| mobility | 10,000 | 20.0 | 8,000 | 2,000 |
| interference | 10,000 | 20.0 | 8,000 | 2,000 |
| malicious | 10,000 | 20.0 | 8,000 | 2,000 |

### Feature Groups (15 Genuine Observables)

| Group | Features | FGA Connection |
|---|---|---|
| MAC Layer | PRR, mac_tx_attempts, mac_tx_success, mac_retries, collisions | Extends P_M |
| Queue/Congestion | queue_len_norm, queue_drops_rate, channel_busy_ratio | Implements P_Q |
| Mobility | link_changes_per_s, neighbor_count | Implements P_η |
| Signal/PHY | rssi_dbm, snr_db, noise_floor_dbm | New — not in FGA |
| QoS | delay_ms, jitter_ms | End-to-end quality |

**Note:** Regime-specific parameters (pps, tx_power_dbm, speed_mps) and analytically derived features (ETX, PM, PQ, Peta, backoff_slots) are excluded to prevent data leakage.

---

## Temporal Regime Calendar

All five regimes are implemented as **direct changes to NS-3.45 simulation parameters** via scheduled callbacks:

| Regime | Window (s) | Label | NS-3.45 Implementation |
|---|---|---|---|
| 1 | 0--140 | benign | Normal 5 pps, 1.5 m/s, 20 dBm |
| 2 | 140--260 | congestion | Traffic raised to 100 pps |
| 3 | 260--380 | mobility | Speed raised to 12.0 m/s |
| 4 | 380--500 | interference | TX power reduced to 4 dBm |
| 5 | 500--620 | malicious | Blackhole/selective-forward nodes activated |

The calendar configuration is stored in `calendar_600_5regimes.json`.

---

## Key Results

| Model | Accuracy | F1-Macro | Mal. F1 | Mob. F1 | Train Time |
|---|---|---|---|---|---|
| **Random Forest** | **84.55%** | **84.54%** | **0.62** | **0.61** | **40.3s** |
| XGBoost | 84.21% | 84.20% | 0.61 | 0.60 | 59.2s |
| SVM | 83.31% | 82.71% | 0.65 | 0.49 | 128.8s |
| MLP | 82.97% | 82.79% | 0.53 | 0.62 | 62.1s |

**Key finding:** Benign, congestion, and interference classes achieve F1 = 1.00 across all models. Malicious and mobility classes exhibit persistent mutual confusion (F1 = 0.53--0.65), reflecting fundamental feature-space overlap between adversarial packet dropping and mobility-induced link instability.

**5-fold cross-validation (XGBoost):**
- Accuracy: 83.94% ± 0.35%
- F1-Macro: 83.93% ± 0.35%
- Malicious F1: 60.68% ± 0.91%
- Mobility F1: 59.18% ± 0.84%

**McNemar's test:**
- RF vs SVM: p < 0.001 ✓
- RF vs MLP: p < 0.001 ✓
- XGB vs SVM: p = 0.013 ✓
- XGB vs MLP: p < 0.001 ✓
- RF vs XGB: p = 0.214 (not significant — equivalent)
- SVM vs MLP: p = 0.353 (not significant — equivalent)

---

## Citation

If you use this dataset or code, please cite:

```bibtex
@article{khan2026mlfga,
  title={ML-FGA: A Machine Learning Framework for Fine-Grained
         Analysis of Packet Loss Causes in MANETs and IoT Networks},
  author={Khan, Muhammad Saleem and Shahzad, Taimur and
          Sharif, Muhammad and Iqbal, Muhammad Ali and
          Kim, Soo Kyun},
  journal={IEEE Access},
  year={2026},
  doi={10.1109/ACCESS.2026.0000000}
}
```

Dataset citation:
```
M. S. Khan, "PL-CausesNS3-50k," IEEE DataPort, 2025.
DOI: 10.21227/86dj-xd45
https://ieee-dataport.org/documents/pl-causesns3-50k
```

---

## License

The dataset is released under the IEEE DataPort terms of use.
The code is released under the MIT License.

---

## Contact

Muhammad Saleem Khan
Department of Computer Science, COMSATS University Islamabad, Attock Campus
Email: saleem_khan@cuiatk.edu.pk
