#!/usr/bin/env python3
"""Compare the five-stage SRAM pipeline with the C++ architectural core."""
import os
from pathlib import Path
import random
import tempfile
from run_rtl_core import ROOT, branch, elf_words, i, program, r, run, s


def random_program(seed):
    rng=random.Random(seed)
    words=[0x000080b7,i(2,0,-1),i(3,0,3)]  # x1 remains the data base
    for _ in range(120):
        a,b,dst=(rng.randrange(0,28),rng.randrange(0,28),rng.randrange(2,28))
        choice=rng.randrange(6)
        if choice == 0:
            words.append(i(dst,a,rng.randrange(-1024,1024)))
        elif choice == 1:
            f3=rng.randrange(8)
            f7=rng.choice([0,0x20]) if f3 in (0,5) else 0
            words.append(r(dst,a,b,f3,f7))
        elif choice == 2:
            words.append(r(dst,a,b,rng.randrange(8),1))
        elif choice == 3:
            words.append(s(a,1,rng.choice([0,4,8]),rng.randrange(3)))
        elif choice == 4:
            words.append(i(dst,1,rng.choice([0,4,8]),rng.choice([0,1,2,4,5]),3))
        else:
            words.append(branch(a,b,8,rng.choice([0,1,4,5,6,7])))
    return words + [0x00000013,0x00000073]


def main():
    with tempfile.TemporaryDirectory(prefix="colab-rtl-pipeline-") as directory:
        temp=Path(directory)
        oracle=temp / "oracle"
        simulation=temp / "pipeline.vvp"
        run([os.environ.get("CXX","g++"),"-std=c++17","-O2","-Wall","-Wextra","-Wpedantic","-Werror",
             "tests/rtl_core_oracle.cpp","-o",str(oracle)])
        args=[os.environ.get("IVERILOG","iverilog")]
        if os.environ.get("IVERILOG_IVL_DIR"):
            args += ["-B",os.environ["IVERILOG_IVL_DIR"]]
        run(args + ["-g2012","-s","rtl_pipeline_tb","-o",str(simulation),"rtl/rv32im/alu32.sv",
                    "rtl/rv32im/muldiv.sv","rtl/rv32im/pipeline.sv","tests/rtl_pipeline_tb.sv"])
        simulations={"sram":simulation}
        for mode,macro in (("axi","WITH_PIPELINE"),("cached_axi","WITH_PIPELINE_CACHE")):
            target=temp / (mode + ".vvp")
            run(args + ["-D" + macro,"-g2012","-s","rtl_core_tb","-o",str(target),
                        "rtl/rv32im/alu32.sv","rtl/rv32im/muldiv.sv","rtl/rv32im/core.sv",
                        "rtl/rv32im/pipeline.sv","rtl/rv32im/pipeline_axi.sv","rtl/rv32im/axi_cache.sv",
                        "rtl/rv32im/cached_core.sv","tests/rtl_core_tb.sv"])
            simulations[mode]=target
        samples={
            "mixed":program(),
            "illegal":[i(2,0,1),0xffffffff],
            "load_misaligned":[i(1,0,1),i(2,1,0,1,3)],
            "store_misaligned":[i(1,0,1),s(0,1,0,1)],
            "branch_misaligned":[branch(0,0,2)],
            "jalr_misaligned":[i(1,0,2),i(2,1,0,0,0x67)],
            # The younger multiply and store must not retire after a bus fault.
            "precise_load_fault":[0x000100b7,i(2,1,0,2,3),r(4,0,0,0,1),0x000081b7,s(4,3,0,2),0x00000073],
            "precise_store_fault":[0x000100b7,s(0,1,0,2),i(2,0,13),0x00000073],
            "fetch_fault":[0x000100b7,i(0,1,0,0,0x67)],
            "flush_branch":[0x000081b7,branch(0,0,8),s(0,3,0,2),i(2,0,42),s(2,3,0,2),0x00000073],
        }
        run(["bash","examples/rtl_smoke/build.sh",str(temp / "elf")])
        samples["compiler_elf"]=elf_words(temp / "elf" / "program.elf")
        samples.update({f"random_{seed}":random_program(seed) for seed in range(6)})
        for name,words in samples.items():
            image=temp / (name + ".hex")
            image.write_text("".join(f"{word:08x}\n" for word in words),encoding="ascii")
            expected=run([str(oracle),str(image)]).stdout.splitlines()
            for mode,sim in simulations.items():
                raw=run([os.environ.get("VVP","vvp"),str(sim),f"+program={image}",f"+words={len(words)}"]).stdout.splitlines()
                actual=[line for line in raw if line.startswith(("RET ","TRAP ","REG ","MEM "))]
                if actual != expected:
                    for index,(a,b) in enumerate(zip(actual,expected)):
                        if a != b:
                            raise SystemExit(f"{name} {mode} mismatch at line {index}: RTL {a!r}; C++ {b!r}")
                    raise SystemExit(f"{name} {mode} length differs: RTL={len(actual)}, C++={len(expected)}")
                if name == "mixed" and mode == "sram":
                    activity=next(line for line in raw if line.startswith("PIPE_ACTIVITY ")).split()
                    if int(activity[1]) == 0 or int(activity[2]) < 32 or int(activity[3]) == 0:
                        raise SystemExit(f"pipeline overlap, multiply/divide or memory stalls missing: {activity}")
                if name == "compiler_elf" and mode == "cached_axi":
                    activity=next(line for line in raw if line.startswith("CACHE_ACTIVITY ")).split()
                    if int(activity[2]) >= int(activity[1]):
                        raise SystemExit(f"pipeline cache recorded no hits: {activity}")
            print(f"PASS RTL pipeline {name}: {len(expected)} records match C++ with SRAM, AXI and cached AXI")


if __name__ == "__main__":
    main()
