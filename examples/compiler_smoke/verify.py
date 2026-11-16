#!/usr/bin/env python3
"""Run compiled C on the real SystemC platform and compare its signature."""
import argparse
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--machine", required=True)
parser.add_argument("--elf", required=True)
args = parser.parse_args()
result = subprocess.run(
    [args.machine, "--interactive", "--elf", args.elf],
    input="on\nrun 8000\nmem 0x60 4\nregs\ncsrs\nquit\n", text=True,
    capture_output=True, timeout=30,
)
if result.returncode or "CPU fault" in result.stdout or "Command failed" in result.stdout:
    raise SystemExit(result.stdout + result.stderr)
match = re.search(r"0x60: ((?:[0-9a-f]{2} ){4})", result.stdout)
if not match or "Cpower=on" not in result.stdout or "x2=0x10000" not in result.stdout:
    raise SystemExit("compiled C did not produce a signature and UART output\n" + result.stdout)
if " exceptions=1 " not in result.stdout or not re.search(r"mcause=0xb\s+mtval=0x0\b", result.stdout):
    raise SystemExit("compiled C did not complete exactly one machine ECALL trap\n" + result.stdout)
# The first status is the powered-on reset snapshot; inspect the running snapshot.
waits = [int(value) for value in re.findall(r"WFI cycles=(\d+)", result.stdout)]
if "IRQs=1 " not in result.stdout or not waits or max(waits) == 0:
    raise SystemExit("compiled C did not wait for and return from the CLINT timer interrupt\n" + result.stdout)
actual = int.from_bytes(bytes.fromhex(match[1]), "little")
expected = 0
for item in (0x12345678, 0x80000000, 0xffffffff, 0x01020304):
    rotate = ((item * 32) % 2**32) + item // 2**27
    expected = ((expected ^ rotate) + ((item % 256) ^ (3 if item < 2**31 else 7))) % 2**32
if actual != expected:
    raise SystemExit(f"compiled C signature mismatch: got {actual:08x}, expected {expected:08x}")
print(f"PASS compiled C: signature=0x{actual:08x}; CLINT/WFI/IRQ and ECALL/handler/MRET; stack restored; ELF -> CPU/Cache/TLM/DIMM -> UART")
