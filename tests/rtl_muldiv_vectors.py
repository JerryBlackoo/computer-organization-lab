#!/usr/bin/env python3
"""Generate reproducible RV32M vectors using Python integer arithmetic."""
import random
import sys

MASK = (1 << 32) - 1
edges = [0, 1, 2, 3, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff]
randomizer = random.Random(0x4D4455)


def signed(value):
    return value if value < 2**31 else value - 2**32


def expected(op, a, b):
    sa, sb = signed(a), signed(b)
    if op <= 3:
        x = sa if op in (1, 2) else a
        y = sb if op == 1 else b
        product = (x * y) & ((1 << 64) - 1)
        return ((product if op == 0 else product >> 32) & MASK,
                32 + int(op == 2 and bool(b & 0x80000000)))
    if b == 0:
        return (MASK if op in (4, 5) else a), 0
    if op in (4, 6) and a == 0x80000000 and b == MASK:
        return (a if op == 4 else 0), 0
    if op in (4, 6):
        quotient, remainder = divmod(abs(sa), abs(sb))
        beats = 32 + int((sa < 0) != (sb < 0) and quotient != 0) + int(sa < 0 and remainder != 0)
        if (sa < 0) != (sb < 0):
            quotient = -quotient
        if sa < 0:
            remainder = -remainder
    else:
        quotient, remainder = divmod(a, b)
        beats = 32
    return ((quotient if op in (4, 5) else remainder) & MASK), beats


with open(sys.argv[1], "w", encoding="ascii") as out:
    for op in range(8):
        pairs = [(a, b) for a in edges for b in edges]
        pairs += [(randomizer.getrandbits(32), randomizer.getrandbits(32)) for _ in range(128)]
        for a, b in pairs:
            value, beats = expected(op, a, b)
            out.write(f"{op:x} {a:08x} {b:08x} {value:08x} {beats:d}\n")
