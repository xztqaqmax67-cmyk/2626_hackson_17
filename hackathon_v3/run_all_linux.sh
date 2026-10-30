#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
g++ -O2 -std=c++17 -Wall -Wextra solver.cpp -o solver
python3 -m unittest discover -s tests -v
python3 score.py --data ../hackathon_data --solver ./solver --engine local -n 4 --solver-time-limit 1 --timeout 300 --out runs/local_v3 "$@"
