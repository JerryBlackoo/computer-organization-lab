"""Verify a guest-built Sv32 address space. This firmware is not an OS."""
import argparse
import hashlib
import re
import struct
import subprocess
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("--machine", required=True)
p.add_argument("--elf", required=True)
p.add_argument("--trace")
a = p.parse_args()
command = [a.machine, "--interactive", "--elf", a.elf]
if a.trace:
    command += ["--trace", a.trace]
# A warm reset must clear CPU/Sv32 state but retain RAM; guest rebuilds tables.
commands = "on\nrun 160000\nmem 0x100 32\nmem 0x180 4\nmem 0x200 120\nmem 0xc000 16\nmem 0x5000 4\ncsrs\nreset\ncsrs\nrun 160000\nmem 0x100 32\nquit\n"
r = subprocess.run(command, input=commands, text=True, capture_output=True, timeout=30)
def fail(message):
    raise SystemExit(message + "\n" + r.stdout + r.stderr)
if r.returncode or "CPU fault" in r.stdout or "Command failed" in r.stdout:
    fail("paged machine failed")
def read(address, count):
    result = bytearray()
    while len(result) < count:
        match = re.search(rf"^0x{address + len(result):x}: ([0-9a-f]{{2}}(?: [0-9a-f]{{2}})*)", r.stdout, re.M)
        if not match:
            fail("missing physical-memory evidence")
        result.extend(bytes.fromhex(match[1]))
    return bytes(result[:count])
expected = (0x53563332, 10, 41, 41, ((42 << 1) ^ 0x55) + 7, 42, 42, 99)
if struct.unpack("<8I", read(0x100, 32)) != expected:
    fail("signature/traps/SUM/MXR/compiled C/SFENCE remap differed")
if struct.unpack("<4I", read(0xc000, 16)) != (42, expected[4], 55, 71):
    fail("user calculation, denied-load precision or A-bit retry differed")
if struct.unpack("<I", read(0x5000, 4))[0] != 99 or struct.unpack("<I", read(0x180, 4))[0] != 0xfeedface:
    fail("D-bit repair failed or protected supervisor memory was corrupted")
elf = Path(a.elf).read_bytes()
shoff = struct.unpack_from("<I", elf, 32)[0]
shsize, shcount = struct.unpack_from("<HH", elf, 46)
sections = [struct.unpack_from("<10I", elf, shoff + i * shsize) for i in range(shcount)]
symbols = {}
for section in sections:
    if section[1] != 2:
        continue
    strings = sections[section[6]]
    names = elf[strings[4]:strings[4] + strings[5]]
    for at in range(section[4], section[4] + section[5], section[9]):
        name, value = struct.unpack_from("<II", elf, at)
        symbols[names[name:names.index(b"\0", name)].decode()] = value
cases = [
    (13, 0x4000c000, "sum_fault", 0), (13, 0x4000d000, "mxr_fault", 0),
    (13, 0x180, "user_load_fault", 0x40000000), (15, 0x180, "user_store_fault", 0x40000000),
    (15, 0x4000a000, "user_code_store_fault", 0x40000000),
    (13, 0x40007000, "user_unmapped_fault", 0x40000000),
    (13, 0x40005000, "user_accessed_fault", 0x40000000),
    (15, 0x40005000, "user_dirty_fault", 0x40000000),
    (12, 0x9000, None, 0x9000), (8, 0, "user_exit_ecall", 0x40000000)]
records = struct.unpack("<30I", read(0x200, 120))
for i, (cause, value, label, delta) in enumerate(cases):
    pc = symbols[label] + delta if label else delta
    if records[i*3:i*3+3] != (cause, value, pc):
        fail(f"trap {i}: wrong cause/stval/exact guest ELF PC")
statuses = [line for line in r.stdout.splitlines() if "cycles=160000 " in line]
if len(statuses) != 2 or any("exceptions=10 " not in line or "mode=S waiting=wfi " not in line for line in statuses):
    fail("cold/warm boot did not finish in S-mode WFI with ten precise traps")
for line in statuses:
    stats = re.search(r"Sv32 walks=(\d+) PTE reads=(\d+) page faults=(\d+)", line)
    if not stats or int(stats[1]) < 10 or int(stats[2]) < 15 or int(stats[3]) != 9:
        fail("no evidence of real page-table transactions or nine page faults")
csrs = [dict((n, int(v, 16)) for n, v in re.findall(r"(\w+)=0x([0-9a-f]+)", line))
        for line in r.stdout.splitlines() if line.startswith("mstatus=")]
if len(csrs) != 2 or csrs[0]["satp"] != 0x80000001 or csrs[0]["scause"] != 8 or csrs[0]["mcause"] != 0:
    fail("final machine/supervisor banks or satp differed")
if any(csrs[1][n] for n in ("satp", "scause", "sepc", "stval", "pmpcfg0", "medeleg")):
    fail("warm reset retained paging/protection/trap state")
signatures = re.findall(r"^0x100: (.*)$", r.stdout, re.M)
if len(signatures) != 2 or signatures[0] != signatures[1]:
    fail("warm boot did not reproduce the cold-boot signature")
if a.trace:
    codes, observations = {}, {}
    wanted = {"satp", "page_walks", "page_table_reads", "page_faults"}
    # Stream large VCD files and accept SystemC's optional [msb:lsb] ranges.
    with Path(a.trace + ".vcd").open() as stream:
        for line in stream:
            fields = line.split()
            if fields and fields[0] == "$var" and fields[4] in wanted:
                codes[fields[4]] = fields[3]
                observations[fields[3]] = set()
            elif len(fields) == 2 and fields[0].startswith("b") and fields[1] in observations:
                if set(fields[0][1:]) <= {"0", "1"}:
                    observations[fields[1]].add(int(fields[0][1:], 2))
    for name, expected_value in (("satp", 0x80000001), ("page_walks", 20), ("page_table_reads", 36), ("page_faults", 9)):
        if name not in codes or expected_value not in observations.get(codes[name], set()):
            fail("VCD missing expected paging signal/value: " + name)
    print("PASS VCD: satp activation, 20 walks, 36 PTE transactions and nine page faults")
print("PASS compiled Sv32 M/S/U: signature=0x53563332, ten precise traps, guest A/D repair, RX/kernel isolation, SUM/MXR, SFENCE remap, cold+warm boot; ELF SHA256=" + hashlib.sha256(elf).hexdigest())
print(statuses[0])
