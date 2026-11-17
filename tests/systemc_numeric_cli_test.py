#!/usr/bin/env python3
"""Check numeric parsing through every actual machine CLI consumer."""
import subprocess
import sys
import tempfile
from pathlib import Path

machine, temp_dir = sys.argv[1:]
failures = []


def run(args, commands=None):
    result = subprocess.run([machine, *map(str, args)], input=commands, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    return result.returncode, result.stdout


def expect(name, condition, output):
    if not condition:
        failures.append(name)
        print(f'FAIL {name}:\n{output}')


with tempfile.TemporaryDirectory(prefix='systemc-numeric-cli-', dir=temp_dir) as tmp:
    binary = Path(tmp) / 'loop.bin'
    binary.write_bytes(bytes.fromhex('63000000'))
    base = ['--bin', binary]
    invalid = ['-1', '-18446744073709518848', '-18446744073709551615',
               '+1', '0x', '0xg', '08x', '4294967296', '18446744073709551616']
    for flag in ('--entry', '--cycles'):
        for value in invalid:
            code, output = run([*base, flag, value, '--interactive'], 'quit\n')
            expect(f'{flag} rejects {value}', code == 1 and 'expected a 32-bit decimal/hex number' in output, output)
    for command in ('step', 'run'):
        for value in invalid:
            code, output = run([*base, '--interactive'], f'on\n{command} {value}\nstatus\nquit\n')
            expect(f'{command} rejects {value}', code == 0 and 'Command failed: expected a 32-bit decimal/hex number' in output
                   and 'cycles=0' in output and 'cycles=1' not in output, output)
    for position in ('address', 'length'):
        for value in invalid:
            args = f'{value} 4' if position == 'address' else f'0 {value}'
            code, output = run([*base, '--interactive'], f'on\nmem {args}\nquit\n')
            expect(f'mem {position} rejects {value}', code == 0 and 'Command failed: expected a 32-bit decimal/hex number' in output, output)
    for value in ('08', '0008', '0x8', '0X8'):
        code, output = run([*base, '--entry', value, '--interactive'], 'on\nstatus\nquit\n')
        expect(f'entry accepts {value}', code == 0 and 'pc=0x8' in output, output)
        code, output = run([*base, '--cycles', value])
        expect(f'cycles accepts {value}', code == 0 and 'cycles=8' in output, output)
        for command in ('step', 'run'):
            code, output = run([*base, '--interactive'], f'on\n{command} {value}\nquit\n')
            expect(f'{command} accepts {value}', code == 0 and 'cycles=8' in output, output)
        code, output = run([*base, '--interactive'], f'on\nmem {value} 04\nmem 0 {value}\nquit\n')
        expect(f'mem accepts {value}', code == 0 and '0x8: 00 00 00 00' in output
               and '0x0: 00 00 00 00 00 00 00 00' in output and 'Command failed:' not in output, output)
    for value in ('4294967295', '0xffffffff'):
        code, output = run([*base, '--cycles', value, '--interactive'], 'quit\n')
        expect(f'cycles accepts uint32 max {value}', code == 0, output)
        code, output = run([*base, '--entry', value, '--cycles', '0'])
        expect(f'entry parses uint32 max {value}', code == 1 and 'boot text is outside DIMM' in output, output)
    for flag in ('--dram-trcd', '--dram-trp', '--dram-tcas'):
        code, output = run([*base, '--timed-dram', flag, '0', '--cycles', '0'])
        expect(f'{flag} rejects zero', code == 1 and 'DRAM timings must be positive' in output, output)
    for flag in ('--dram-ta', '--dram-bus'):
        code, output = run([*base, '--interleaved-dram', flag, '0', '--cycles', '0'])
        expect(f'{flag} rejects zero', code == 1 and 'access time must be positive' in output, output)
    code, output = run([*base, '--interleaved-dram', '--dram-ta', '3', '--cycles', '0'])
    expect('interleaved rejects incompatible bank count', code == 1 and 'ta must equal banks' in output, output)
    code, output = run([*base, '--interleaved-dram', '--dram-ta', '3', '--dram-no-bank-check', '--cycles', '8'])
    expect('interleaved supports explicit nonideal timing', code == 0 and 'cycles=8' in output, output)
    code, output = run([*base, '--interleaved-dram', '--timed-dram', '--cycles', '0'])
    expect('DRAM modes are mutually exclusive', code == 1 and 'mutually exclusive' in output, output)
    for flag in ('--icache-hit-cycles', '--dcache-hit-cycles'):
        code, output = run([*base, flag, '3', '--cycles', '0'])
        expect(f'{flag} rejects 3', code == 1 and 'cache hit must take 1 or 2 cycles' in output, output)

if failures:
    print(f'FAIL numeric CLI: {len(failures)} cases failed: {", ".join(failures)}')
    sys.exit(1)
print('PASS numeric CLI: option, run, step, mem decimal/hex, malformed and uint32 boundaries')
