"""Execute actual M/S/U firmware, then check independent results and ELF PCs."""
import argparse
import re
import struct
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--machine", required=True)
parser.add_argument("--elf", required=True)
parser.add_argument("--trace")
args = parser.parse_args()
command = [args.machine, "--interactive", "--elf", args.elf]
if args.trace:
    command += ["--trace", args.trace]
run = subprocess.run(command, input="on\nrun 12000\nregs\nmem 0x100 84\nmem 0xc000 8\ncsrs\nreset\ncsrs\nquit\n",
                     text=True, capture_output=True, timeout=30)
if run.returncode or "CPU fault" in run.stdout or "Command failed" in run.stdout:
    raise SystemExit(run.stdout + run.stderr)

def fail(message):
    raise SystemExit(message + "\n" + run.stdout + run.stderr)

def read(address, count):
    result = bytearray()
    while len(result) < count:
        match = re.search(rf"^0x{address + len(result):x}: ([0-9a-f]{{2}}(?: [0-9a-f]{{2}})*)", run.stdout, re.M)
        if not match:
            fail("missing guest memory")
        result.extend(bytes.fromhex(match[1]))
    return result[:count]

records = struct.unpack("<21I", read(0x100, 84))
signature, traps, s_timer, m_timer, m_calls, result = records[:6]
if (signature, traps, s_timer, m_timer, m_calls, result) != (0x53555052, 3, 1, 1, 3, ((21 + 8 + 17) << 1) ^ 0x5a):
    fail("M/S/U signature, trap counts, timer forwarding or nested call result differed")
checksum = 0
for value in range(7, 11):
    checksum = (checksum << 3) ^ value
if struct.unpack("<2I", read(0xc000, 8)) != (result, checksum):
    fail("compiled user C calculation differed")
if records[6] != 8 or records[8] & 0x100 or records[10] & 0x1800 != 0x800:
    fail("U -> S or S -> M saved the wrong privilege/cause")
if records[13:15] != (1, 0x305022f3) or records[17] & 0x100 != 0x100:
    fail("illegal CSR or S timer did not preserve the expected trap context")
if not (0xc000 < records[19] <= 0xd000 and 0xe000 < records[20] < 0xf000):
    fail("user/supervisor C stacks were not distinct")

elf = Path(args.elf).read_bytes()
phoff, shoff = struct.unpack_from("<II", elf, 28)
phsize, phcount, shsize, shcount = struct.unpack_from("<HHHH", elf, 42)
sections = [struct.unpack_from("<10I", elf, shoff + i * shsize) for i in range(shcount)]
symbols = {}
for section in sections:
    if section[1] != 2:
        continue
    strings = sections[section[6]]
    names = elf[strings[4]:strings[4] + strings[5]]
    for offset in range(section[4], section[4] + section[5], section[9]):
        name, address = struct.unpack_from("<II", elf, offset)
        end = names.index(b"\0", name)
        symbols[names[name:end].decode()] = address

def instruction(pc):
    for i in range(phcount):
        kind, offset, va, _, size, _, _, _ = struct.unpack_from("<8I", elf, phoff + phsize * i)
        if kind == 1 and va <= pc and pc + 4 <= va + size:
            return struct.unpack_from("<I", elf, offset + pc - va)[0]
    fail("saved trap PC is outside file-backed ELF code")

if records[7] != symbols["supervisor_user_exit_ecall"] or instruction(records[7]) != 0x73:
    fail("S handler did not save the exact user ECALL PC")
if instruction(records[9]) != 0x305022f3 or instruction(records[11]) != 0x73:
    fail("illegal CSR or nested machine ECALL PC differed from ELF")
if records[15] != symbols["supervisor_wfi_after"] or records[16] != records[15]:
    fail("machine timer/S forwarding did not preserve the WFI resume PC")
if " exceptions=6 " not in run.stdout or " IRQs=2 " not in run.stdout or "S!power=on" not in run.stdout:
    fail("firmware did not complete six exceptions, two IRQs and UART S!")
waits = [int(value) for value in re.findall(r"WFI cycles=(\d+)", run.stdout)]
if not waits or max(waits) == 0 or " mode=S " not in run.stdout:
    fail("S-mode did not actually sleep and resume")
csr_lines = [line for line in run.stdout.splitlines() if line.startswith("mstatus=")]
if len(csr_lines) != 2:
    fail("missing CSR output before/after reset")
values = dict((name, int(value, 16)) for name, value in re.findall(r"(\w+)=0x([0-9a-f]+)", csr_lines[0]))
if values["scause"] != 8 or values["mcause"] != 9 or values["satp"] != 0 or values["mscratch"] != 0x10000:
    fail("final CSR banks, Bare mode or machine stack differed")
reset = dict((name, int(value, 16)) for name, value in re.findall(r"(\w+)=0x([0-9a-f]+)", csr_lines[1]))
if any(reset[name] for name in ("medeleg", "mideleg", "scause", "sepc", "stval", "stvec", "sscratch", "sip", "sie", "satp")):
    fail("reset did not clear supervisor/delegation state")
print("PASS compiled M/S/U C: CLINT -> M timer -> STIP/S handler, S WFI/SRET, U ECALL -> S C -> M ECALL -> U, illegal CSR recovery; signature=0x53555052")
