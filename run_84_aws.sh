#!/bin/bash
# ============================================================
# ML-FGA — Run All 84 NS-3.45 Simulation Runs
# ============================================================
# Usage: bash run_84_aws.sh
# Runs 4 simulations in parallel using xargs
# Output: ~/ns3-output-v2/groupA, groupB, groupC
#
# Group A: 20 nodes, 150x150m, seeds  1-28
# Group B: 30 nodes, 200x200m, seeds 29-56
# Group C: 40 nodes, 250x250m, seeds 57-84
#
# Each run: 620 seconds simulation time
# 5 regimes implemented in simulator:
#   0-140s:   benign       (5pps, 1.5m/s, 20dBm)
#   140-260s: congestion   (100pps)
#   260-380s: mobility     (12m/s)
#   380-500s: interference (4dBm TX power)
#   500-620s: malicious    (blackhole/selective-forward)
# ============================================================

NS3="/home/ubuntu/ns-allinone-3.45/ns-3.45"
OUT="/home/ubuntu/ns3-output-v2"

mkdir -p $OUT/groupA $OUT/groupB $OUT/groupC

# ── Single run function ──────────────────────────────────────
run_one() {
    SEED=$1
    NODES=$2
    AREA=$3
    GRP=$4
    NS3="/home/ubuntu/ns-allinone-3.45/ns-3.45"
    OUT="/home/ubuntu/ns3-output-v2"

    # Skip if already completed
    if [ -f "$OUT/$GRP/run_${SEED}_tx.csv" ]; then
        echo "[$GRP] seed=$SEED already done — skipping"
        return
    fi

    echo "[$GRP] seed=$SEED starting (nodes=$NODES area=${AREA}x${AREA}m)..."

    cd "$NS3"
    ./ns3 run "scratch/plc_scenario_final \
        --outDir=$OUT/$GRP \
        --seed=$SEED \
        --runId=$SEED \
        --nNodes=$NODES \
        --areaSize=$AREA \
        --simSeconds=620" \
        > "$OUT/$GRP/log_${SEED}.txt" 2>&1

    # Verify output
    if [ -f "$OUT/$GRP/run_${SEED}_tx.csv" ]; then
        TX=$(wc -l < "$OUT/$GRP/run_${SEED}_tx.csv")
        RX=$(wc -l < "$OUT/$GRP/run_${SEED}_rx.csv")
        echo "[$GRP] seed=$SEED DONE (tx=$TX rx=$RX)"
    else
        echo "[$GRP] seed=$SEED FAILED — check log_${SEED}.txt"
    fi
}
export -f run_one

# ── Group A: 20 nodes, 150x150m, seeds 1-28 ─────────────────
echo ""
echo "=== Group A: 20 nodes, 150x150m, seeds 1-28 ==="
echo "    Running 4 parallel jobs..."
seq 1 28 | xargs -I{} -P 4 bash -c 'run_one {} 20 150 groupA'

echo ""
echo "=== Group A complete ==="
echo "    Files: $(ls $OUT/groupA/*_tx.csv 2>/dev/null | wc -l) /28"

# ── Group B: 30 nodes, 200x200m, seeds 29-56 ────────────────
echo ""
echo "=== Group B: 30 nodes, 200x200m, seeds 29-56 ==="
echo "    Running 4 parallel jobs..."
seq 29 56 | xargs -I{} -P 4 bash -c 'run_one {} 30 200 groupB'

echo ""
echo "=== Group B complete ==="
echo "    Files: $(ls $OUT/groupB/*_tx.csv 2>/dev/null | wc -l) /28"

# ── Group C: 40 nodes, 250x250m, seeds 57-84 ────────────────
echo ""
echo "=== Group C: 40 nodes, 250x250m, seeds 57-84 ==="
echo "    Running 4 parallel jobs..."
seq 57 84 | xargs -I{} -P 4 bash -c 'run_one {} 40 250 groupC'

echo ""
echo "=== Group C complete ==="
echo "    Files: $(ls $OUT/groupC/*_tx.csv 2>/dev/null | wc -l) /28"

# ── Final summary ────────────────────────────────────────────
echo ""
echo "============================================"
echo "=== ALL RUNS COMPLETE ==="
echo "============================================"
echo "GroupA: $(ls $OUT/groupA/*_tx.csv 2>/dev/null | wc -l) /28"
echo "GroupB: $(ls $OUT/groupB/*_tx.csv 2>/dev/null | wc -l) /28"
echo "GroupC: $(ls $OUT/groupC/*_tx.csv 2>/dev/null | wc -l) /28"
echo "Total:  $(ls $OUT/*/*_tx.csv 2>/dev/null | wc -l) /84"
echo ""
echo "Output directory: $OUT"
echo "Next step: python3 ~/aggregate_v2.py"
echo "============================================"
