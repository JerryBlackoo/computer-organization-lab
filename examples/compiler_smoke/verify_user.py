"""Verify a real compiled U-mode program and guest M-mode trap handler."""
import argparse
import re
import subprocess
import struct
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--machine", required=True)
parser.add_argument("--elf", required=True)
args = parser.parse_args()
run = subprocess.run(
    [args.machine, "--interactive", "--elf", args.elf],
    input="on\nuart-input Q\nrun 10000\nmem 0x60 8\nmem 0x100 96\nmem 0x8800 4\nmem 0xa000 16\ncsrs\nreset\ncsrs\nquit\n",
    text=True, capture_output=True, timeout=30,
)
if run.returncode or "CPU fault" in run.stdout or "Command failed" in run.stdout:
    raise SystemExit(run.stdout + run.stderr)

def read(address, count):
    result = bytearray()
    while len(result) < count:
        match = re.search(rf"^0x{address + len(result):x}: ([0-9a-f]{{2}}(?: [0-9a-f]{{2}})*)", run.stdout, re.M)
        if not match:
            raise SystemExit("missing guest memory output\n" + run.stdout)
        result.extend(bytes.fromhex(match[1]))
    return result[:count]

def words(data):
    return [int.from_bytes(data[i:i+4], "little") for i in range(0, len(data), 4)]

signature, count = words(read(0x60, 8))
record_words = words(read(0x100, 96))
records = [record_words[i:i+4] for i in range(0, 24, 4)]
expected = 0
for item in (1, 7, 31, 255):
    expected = ((expected << 3) ^ item) & 0xffffffff
if signature != 0x50524f54 or count != 6 or words(read(0x8800, 4)) != [0xcafebabe] or words(read(0xa000, 16))[1] != expected:
    raise SystemExit("U-mode signature, trap count, protected data or user calculation differed\n" + run.stdout)
if [record[0] for record in records] != [8, 2, 5, 7, 1, 8] or [record[2] for record in records] != [0, 0x305022f3, 0x40001000, 0x8800, 0x8000, 0]:
    raise SystemExit("U-mode trap cause/value sequence differed\n" + run.stdout)
if any((not 0x9000 <= pc < 0xa000 if cause != 1 else pc != 0x8000) or pc % 4 or status & (3 << 11)
       for cause, pc, _, status in records):
    raise SystemExit("U-mode traps saved incorrect PC or previous privilege\n" + run.stdout)
elf = Path(args.elf).read_bytes()
phoff = struct.unpack_from("<I", elf, 28)[0]
phsize, phcount = struct.unpack_from("<HH", elf, 42)
def instruction(pc):
    for i in range(phcount):
        kind, offset, va, _, size, _, _, _ = struct.unpack_from("<8I", elf, phoff + phsize*i)
        if kind == 1 and va <= pc and pc + 4 <= va + size:
            return struct.unpack_from("<I", elf, offset + pc - va)[0]
    raise SystemExit("saved trap PC is outside file-backed ELF code")
code = [instruction(record[1]) for record in records]
if code[0] != 0x73 or code[1] != 0x305022f3 or code[2] & 0x707f != 0x2003 or code[3] & 0x707f != 0x2023 or code[5] != 0x73:
    raise SystemExit("saved trap PCs do not point to expected ELF instructions")
stack = re.search(r"mscratch=0x([0-9a-f]+)", run.stdout)
if not stack or not 0xa000 < int(stack[1], 16) <= 0xb000:
    raise SystemExit("machine trap did not preserve a separate user stack")
if " exceptions=6 " not in run.stdout or "U!power=on" not in run.stdout or "uart status=7" not in run.stdout:
    raise SystemExit("guest did not complete six traps, preserve UART input and output U!\n" + run.stdout)
after_reset = run.stdout.rsplit("mstatus=", 1)[1]
if any(f"{name}=0x0" not in after_reset for name in ("pmpcfg0", "pmpaddr0", "pmpaddr1", "pmpaddr2")):
    raise SystemExit("hardware reset did not clear PMP configuration")
print("PASS compiled U-mode C: PMP code/data/stack, ECALL roundtrip, CSR/MMIO/kernel-write/fetch rejection, protected data intact; signature=0x50524f54")
