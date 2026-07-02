#!/usr/bin/env python3
# Parametric tuner for the pure-ECN controller in rtt_template.c.
# Usage: tune_ecn.py <dec_permille> <ai_shift> <gate>
#   dec_permille : per-CNP multiplicative decrease, e.g. 990 => x0.990, 995 => x0.995
#   ai_shift     : additive-increase step = (AI >> ai_shift); smaller shift => bigger step
#   gate         : apply the AI increase every <gate>-th TX event
# Edits are idempotent via regex (re-runnable to any value). Backup .pre_pureecn is left untouched.
import sys, re

f = "/home/ubuntu/doca_devel/applications/pcc/device/rp/rtt_template/algo/rtt_template.c"
dec_permille = int(sys.argv[1]); ai_shift = int(sys.argv[2]); gate = int(sys.argv[3])
s = open(f).read()

# 1) CNP multiplicative-decrease factor (per-mille)
s, n1 = re.subn(r"#define ECN_CNP_DEC_FACTOR \(\(\(1 << 16\) \* \d+\) / \d+\)",
                "#define ECN_CNP_DEC_FACTOR (((1 << 16) * %d) / 1000)" % dec_permille, s)
# 2) AI increase step
s, n2 = re.subn(r"cur_rate \+= \(AI >> \d+\);",
                "cur_rate += (AI >> %d);" % ai_shift, s)
# 3) AI increase gate
s, n3 = re.subn(r"\(\+\+g_tx_inc % \d+\) == 0",
                "(++g_tx_inc %% %d) == 0" % gate, s)

assert n1 == 1 and n2 == 1 and n3 == 1, "anchors: dec=%d ai=%d gate=%d" % (n1, n2, n3)
open(f, "w").write(s)
print("OK: dec=x0.%03d  ai_step=(AI>>%d)  gate=every-%d-TX" % (dec_permille, ai_shift, gate))
