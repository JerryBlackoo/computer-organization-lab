#!/usr/bin/env python3
"""Compare RTL AXI-Lite retirement/state with the existing C++ RV32 core."""
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def r(rd, a, b, f3=0, f7=0):
    return f7 << 25 | b << 20 | a << 15 | f3 << 12 | rd << 7 | 0x33


def i(rd, a, imm, f3=0, op=0x13):
    return (imm & 0xfff) << 20 | a << 15 | f3 << 12 | rd << 7 | op


def s(source, base, offset, f3):
    value = offset & 0xfff
    return (value >> 5) << 25 | source << 20 | base << 15 | f3 << 12 | (value & 31) << 7 | 0x23


def branch(a, b, offset, f3=0):
    value = offset & 0x1fff
    return ((value >> 12) & 1) << 31 | ((value >> 5) & 63) << 25 | b << 20 | a << 15 | f3 << 12 | ((value >> 1) & 15) << 8 | ((value >> 11) & 1) << 7 | 0x63


def program():
    words = [0x000080b7, i(2, 0, -1), i(3, 0, 3), 0x80000237, 0x7ffff2b7,
             i(5, 5, -1), i(0, 0, 123)]
    # All base R-type ALU functions, with SUB and SRA separately.
    for f3 in range(8):
        words.append(r(6 + f3, 2, 3, f3))
    words += [r(14, 4, 3, 0, 0x20), r(15, 4, 3, 5, 0x20)]
    # All immediate forms, including arithmetic and logical shifts.
    words += [i(16, 2, -7), i(17, 3, 31, 1), i(18, 2, 7, 2), i(19, 2, 7, 3),
              i(20, 2, -10, 4), i(21, 4, 31, 5), i(22, 4, 0x41f, 5),
              i(23, 3, 7, 6), i(24, 2, 17, 7)]
    for f3 in range(8):
        words += [r(6 + f3, 4, 2, f3, 1), r(14 + f3, 2, 3, f3, 1), r(22 + f3, 3, 0, f3, 1)]
    words += [s(2, 1, 0, 0), s(2, 1, 2, 1), s(4, 1, 4, 2),
              i(6, 1, 0, 0, 3), i(7, 1, 0, 4, 3), i(8, 1, 2, 1, 3),
              i(9, 1, 2, 5, 3), i(10, 1, 4, 2, 3), s(10, 1, 8, 2),
              branch(2, 2, 8), i(20, 0, 99), i(20, 0, 11),
              0x00800aef, i(21, 0, 99), 0x00000f17, i(30, 30, 16),
              i(22, 30, 0, 0, 0x67), i(22, 0, 99), i(23, 0, 17), 0x0ff0000f]
    randomizer = random.Random(0xA81)
    for _ in range(180):
        rd, a, b = (randomizer.randrange(6, 30), randomizer.randrange(0, 30), randomizer.randrange(0, 30))
        f3 = randomizer.randrange(8)
        f7 = randomizer.choice([0, 1, 0x20]) if f3 in (0, 5) else randomizer.choice([0, 1])
        words.append(r(rd, a, b, f3, f7))
    return words + [0x00000073]


def run(command, **kwargs):
    return subprocess.run(command, cwd=ROOT, check=True, text=True, capture_output=True, **kwargs)


def elf_words(path):
    data = path.read_bytes()
    if data[:5] != b"\x7fELF\x01" or data[5] != 1:
        raise ValueError("expected little-endian ELF32")
    phoff = struct.unpack_from("<I", data, 28)[0]
    phsize, phnum = struct.unpack_from("<HH", data, 42)
    image = bytearray()
    for n in range(phnum):
        kind, offset, vaddr, paddr, filesz, memsz, _, _ = struct.unpack_from("<IIIIIIII", data, phoff + n * phsize)
        if kind != 1 or not memsz:
            continue
        if vaddr != paddr or paddr + memsz > 0x8000 or offset + filesz > len(data) or filesz > memsz:
            raise ValueError("ELF segment outside smoke RAM or not identity mapped")
        if len(image) < paddr + memsz:
            image.extend(bytes(paddr + memsz - len(image)))
        image[paddr:paddr + filesz] = data[offset:offset + filesz]
    image.extend(bytes((-len(image)) % 4))
    return [int.from_bytes(image[n:n+4], "little") for n in range(0, len(image), 4)]


def main():
    compiler = os.environ.get("IVERILOG", "iverilog")
    runner = os.environ.get("VVP", "vvp")
    ivl_dir = os.environ.get("IVERILOG_IVL_DIR")
    with tempfile.TemporaryDirectory(prefix="colab-rtl-core-") as directory:
        temp = Path(directory)
        exe = temp / "oracle"
        run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             "tests/rtl_core_oracle.cpp", "-o", str(exe)])
        args = [compiler]
        if ivl_dir:
            args += ["-B", ivl_dir]
        simulations = {}
        for cache in (False, True):
            simulation = temp / ("cached.vvp" if cache else "core.vvp")
            run(args + (["-DWITH_CACHE"] if cache else []) + ["-g2012", "-s", "rtl_core_tb", "-o", str(simulation),
                "rtl/rv32im/alu32.sv", "rtl/rv32im/muldiv.sv", "rtl/rv32im/core.sv",
                "rtl/rv32im/pipeline.sv", "rtl/rv32im/pipeline_axi.sv", "rtl/rv32im/axi_cache.sv",
                "rtl/rv32im/cached_core.sv", "tests/rtl_core_tb.sv"])
            simulations[cache] = simulation
        samples = {
            "mixed": program(),
            "illegal": [i(2, 0, 1), 0xffffffff],
            "load_misaligned": [i(1, 0, 1), i(2, 1, 0, 1, 3)],
            "store_misaligned": [i(1, 0, 1), s(0, 1, 0, 1)],
            "branch_misaligned": [branch(0, 0, 2)],
            "jalr_misaligned": [i(1, 0, 2), i(2, 1, 0, 0, 0x67)],
        }
        run(["bash", "examples/rtl_smoke/build.sh", str(temp / "elf")])
        samples["compiler_elf"] = elf_words(temp / "elf" / "program.elf")
        for name, words in samples.items():
            image = temp / (name + ".hex")
            image.write_text("".join(f"{word:08x}\n" for word in words), encoding="ascii")
            oracle = run([str(exe), str(image)]).stdout.splitlines()
            for cache, simulation in simulations.items():
                raw = run([runner, str(simulation), f"+program={image}", f"+words={len(words)}"]).stdout.splitlines()
                actual = [line for line in raw if line.startswith(("RET ", "TRAP ", "REG ", "MEM "))]
                if actual != oracle:
                    for index, (a, b) in enumerate(zip(actual, oracle)):
                        if a != b:
                            raise SystemExit(f"{name} cache={cache} mismatch at line {index}: RTL {a!r}; C++ {b!r}")
                    raise SystemExit(f"{name} cache={cache} length differs: RTL={len(actual)}, C++={len(oracle)}")
                if cache and name == "compiler_elf":
                    activity = next(line for line in raw if line.startswith("CACHE_ACTIVITY ")).split()
                    if int(activity[2]) >= int(activity[1]):
                        raise SystemExit(f"RTL cache recorded no hits: {activity}")
            if name == "compiler_elf":
                expected = ["MEM 00008000 fffffffc", "MEM 00008004 ffffffff", "MEM 00008008 aaaaaaae"]
                if oracle[-3:] != expected:
                    raise SystemExit(f"compiled C signature differed: {oracle[-3:]}")
            print(f"PASS RTL core {name}: {len(oracle)} retirement/state records match C++, with and without cache")


if __name__ == "__main__":
    main()
