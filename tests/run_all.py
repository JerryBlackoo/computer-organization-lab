#!/usr/bin/env python3
"""Lab 1 test runner — RV32I emulator + MESI simulator acceptance tests.

Usage:
    python3 tests/run_all.py --emu ./rv32emu --mesi ./mesi_sim
    python3 tests/run_all.py --emu ./rv32emu          # RV32I only
    python3 tests/run_all.py --mesi ./mesi_sim        # MESI only

Exit code: 0 if all selected tests pass, 1 otherwise.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
RV32I_DIR = HERE / "rv32i"
MESI_DIR = HERE / "mesi"

RV32I_CASES = {
    "t1_add":       {"reg": "x3",  "value": 12},
    "t2_branch":    {"reg": "x5",  "value": 5},
    "t3_mem":       {"reg": "x2",  "value": 42},
    "t4_fib":       {"reg": "x11", "value": 55},
    "t5_illegal":   {"reg": None,  "value": None, "exit": 1},
}

MESI_CASES = {
    "t1_single":          {"exit": 0},
    "t2_shared":          {"exit": 0},
    "t3_invalidate":      {"exit": 0},
    "t4_false_sharing":   {"exit": 0},
    "t5_violation":       {"exit": 0},  # correct impl: no violation
}


def run_rv32i(emu: str, name: str) -> tuple[int, str]:
    """Run one hex program. Returns (exit_code, stdout)."""
    hexfile = RV32I_DIR / f"{name}.hex"
    cmd = [emu, "--hex", "--dump-regs", "--max-cycles", "100000", str(hexfile)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return proc.returncode, proc.stdout


def check_rv32i(emu: str) -> bool:
    ok = True
    for name, expect in RV32I_CASES.items():
        rc, out = run_rv32i(emu, name)
        if expect.get("exit") is not None:
            passed = rc == expect["exit"]
            print(f"[RV32I] {name}: exit={rc} expected={expect['exit']} "
                  f"{'PASS' if passed else 'FAIL'}")
            ok &= passed
            continue
        # parse register dump: "x0=... x1=... " line
        m = re.search(rf"\b{expect['reg']}=([0-9a-fA-F]+)", out)
        if m is None:
            print(f"[RV32I] {name}: register {expect['reg']} not found in output "
                  f"FAIL")
            ok = False
            continue
        value = int(m.group(1), 16)
        passed = rc == 0 and value == expect["value"]
        print(f"[RV32I] {name}: exit={rc} {expect['reg']}=0x{value:x} "
              f"expected=0x{expect['value']:x} {'PASS' if passed else 'FAIL'}")
        ok &= passed
    return ok


def run_mesi(mesi: str, name: str) -> tuple[int, str]:
    trace = MESI_DIR / f"{name}.trace"
    cmd = [mesi, "--cores", "4", "--verbose", str(trace)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return proc.returncode, proc.stdout + proc.stderr


def check_mesi(mesi: str) -> bool:
    ok = True
    for name, expect in MESI_CASES.items():
        rc, out = run_mesi(mesi, name)
        violation = "VIOLATION" in out
        stats_ok = re.search(r"hit_rate=([0-9.]+)", out) is not None
        passed = rc == expect["exit"] and not violation and stats_ok
        print(f"[MESI] {name}: exit={rc} violation={violation} "
              f"stats={stats_ok} {'PASS' if passed else 'FAIL'}")
        ok &= passed
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--emu", default=None)
    ap.add_argument("--mesi", default=None)
    args = ap.parse_args()
    if args.emu is None and args.mesi is None:
        ap.error("need at least one of --emu / --mesi")
    results = []
    if args.emu:
        results.append(("RV32I", check_rv32i(args.emu)))
    if args.mesi:
        results.append(("MESI", check_mesi(args.mesi)))
    for name, passed in results:
        print(f"== {name}: {'ALL PASS' if passed else 'FAILURES'} ==")
    return 0 if all(p for _, p in results) else 1


if __name__ == "__main__":
    sys.exit(main())
