#!/usr/bin/env python3
"""Exercise the actual machine CLI and its binary/ELF boot paths."""
import resource
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

machine = sys.argv[1]


def run(*args, commands=None, memory_limit=None):
    def limit_memory():
        resource.setrlimit(resource.RLIMIT_AS, (memory_limit, memory_limit))

    result = subprocess.run(
        [machine, *map(str, args)], input=commands, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90,
        preexec_fn=limit_memory if memory_limit else None,
    )
    return result.returncode, result.stdout


def accepted(path, entry, last_address, last_bytes, default_entry=False):
    entry_option = () if default_entry else ('--entry', hex(entry))
    code, output = run('--bin', path, *entry_option, '--interactive',
                       commands=f'on\nstatus\nmem {hex(entry)} 4\nmem {hex(last_address)} 4\nquit\n')
    for expected in (f'pc=0x{entry:x}', f'0x{entry:x}: 63 00 00 00',
                     f'0x{last_address:x}: {last_bytes}'):
        if code != 0 or expected not in output:
            raise AssertionError(f'valid image {path.name}: expected {expected!r}, exit {code}\n{output}')


def rejected(path, entry, message, memory_limit=None):
    code, output = run('--bin', path, '--entry', entry, '--cycles', '0', memory_limit=memory_limit)
    if code != 1 or f'FAIL: {message}' not in output:
        raise AssertionError(f'invalid image {path.name}, entry {entry}: exit {code}\n{output}')


def elf_fixture():
    data = bytearray(0x104)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 243, 1, 0x8000, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into('<IIIIIIII', data, 52, 1, 0x100, 0x8000, 0x8000, 4, 4, 5, 4)
    struct.pack_into('<I', data, 0x100, 0x00000063)
    return data


with tempfile.TemporaryDirectory(prefix='systemc-binary-cli-', dir=sys.argv[2]) as tmp:
    root = Path(tmp)
    small = root / 'small.bin'
    small.write_bytes(bytes.fromhex('63000000'))
    accepted(small, 0x8000, 0x8000, '63 00 00 00', default_entry=True)

    large = root / 'large.bin'
    payload = bytearray(0x10004)
    payload[:4] = bytes.fromhex('63000000')
    payload[-4:] = bytes.fromhex('78563412')
    large.write_bytes(payload)
    accepted(large, 0x8000, 0x18000, '78 56 34 12')

    exact = root / 'exact.bin'
    payload = bytearray(0x18000)
    payload[:4] = bytes.fromhex('63000000')
    payload[-4:] = bytes.fromhex('efbeadde')
    exact.write_bytes(payload)
    accepted(exact, 0x28000, 0x3fffc, 'ef be ad de')
    accepted(small, 0x3fffc, 0x3fffc, '63 00 00 00')
    # Run instructions, in addition to checking that the images were loaded.
    for name, length, entry in (('large-exec', 0x10004, 0x8000),
                                ('exact-exec', 0x18000, 0x28000),
                                ('full-exec', 0x40000, 0)):
        program = bytearray(length)
        struct.pack_into('<III', program, 0, 0x02a00093, 0x06102023, 0x00000063)
        program[-4:] = bytes.fromhex('efcdab89')
        path = root / name
        path.write_bytes(program)
        code, output = run('--bin', path, '--entry', hex(entry), '--interactive',
                           commands='on\nrun 300\nmem 0x60 4\nmem 0x3fffc 4\nquit\n'
                           if entry + length == 0x40000 else
                           f'on\nrun 300\nmem 0x60 4\nmem {hex(entry + length - 4)} 4\nquit\n')
        if (code != 0 or not any('cycles=300' in line and 'exceptions=0' in line for line in output.splitlines()) or
                '0x60: 2a 00 00 00' not in output or
                f'0x{entry + length - 4:x}: ef cd ab 89' not in output):
            raise AssertionError(f'{name} execution: exit {code}\n{output}')
    rejected(exact, '0x28004', 'boot text is outside DIMM')
    rejected(small, '0x40000', 'boot text is outside DIMM')
    rejected(small, '0xfffffffc', 'boot text is outside DIMM')
    rejected(small, '0xffffffff', 'boot text is outside DIMM')
    rejected(small, '0x8001', 'boot text is outside DIMM')
    rejected(small, '0x100000000', 'expected a 32-bit decimal/hex number')

    for size in (0, 1, 3, 5):
        bad = root / f'bad-{size}.bin'
        bad.write_bytes(bytes(size))
        rejected(bad, '0', 'raw RV32 image must be at least 4 bytes, multiple of 4')

    huge = root / 'huge.bin'
    with huge.open('wb') as stream:
        stream.truncate(0x100000004)  # sparse, larger than the 32-bit address space
    rejected(huge, '0', 'boot text is outside DIMM', memory_limit=128 * 1024 * 1024)

    elf = root / 'fixture.elf'
    elf.write_bytes(elf_fixture())
    rejected(elf, '0x8000', 'ELF is not a flat binary; use --elf instead of --bin')
    code, output = run('--elf', elf, '--interactive',
                       commands='on\nstatus\nmem 0x8000 4\nquit\n')
    if code != 0 or 'pc=0x8000' not in output or '0x8000: 63 00 00 00' not in output:
        raise AssertionError(f'ELF compatibility: exit {code}\n{output}')
    code, output = run('--elf', elf, '--entry', '0x8000', '--cycles', '0')
    if code != 1 or 'FAIL: ELF supplies its own entry; omit --entry' not in output:
        raise AssertionError(f'ELF entry rejection: exit {code}\n{output}')

print('PASS: actual CLI binary range, lengths, pre-allocation rejection and ELF compatibility')
