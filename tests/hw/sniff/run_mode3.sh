#!/bin/bash
# Preamble detection campaign, EU mode 3 (RAILtest mode-3 image on R, W, I).
# usage: run_mode3.sh <out dir>     (about 40 min)
set -eu
H=$(cd "$(dirname "$0")" && pwd); O=$1
python3 "$H/sniff.py" s1_settle  "$O" --lp 960 --x 100,1500:5500:500 --n 30
python3 "$H/sniff.py" s2_remain  "$O" --lp 960 --x 6200:7150:25 --n 20
python3 "$H/sniff.py" s2_short   "$O" --lp 96  --x 300:1400:25 --n 20
python3 "$H/sniff.py" s3_sir     "$O" --lp 960 --x 3000 --pW 30 --pI -150,-110,-90,-70,-50,-30,0,30,60,100,150 --n 30
