#!/usr/bin/env python3
"""Generate .clangd from the toolchain that actually built this project.

Runs the real compile command from build/compile_commands.json with -E -v to
extract the exact system include dirs (libc, libstdc++, gcc builtins), so the
result matches the build regardless of chip, IDF version, newlib vs picolibc,
or toolchain location.

Invoked by `idf.py clangd` (and automatically after `idf.py build`).

Requires clangd >= 20 (LLVM 20 added the Xtensa backend).
"""

import json
import os
import re
import shlex
import subprocess
import sys

REMOVE_FLAGS = [
    "-fno-shrink-wrap",
    "-fstrict-volatile-bitfields",
    "-fno-tree-switch-conversion",
    "-mlongcalls",
    "-fmacro-prefix-map=*",
    "-fzero-init-padding-bits=all",
    "-fno-malloc-dce",
]


def load_db(project_dir):
    path = os.path.join(project_dir, "build", "compile_commands.json")
    if not os.path.exists(path):
        sys.exit(f"{path} not found; run `idf.py build` first")
    with open(path) as f:
        return json.load(f)


def pick_entry(entries):
    for e in entries:
        if e.get("file", "").endswith((".cpp", ".cc", ".cxx")):
            return e
    for e in entries:
        if e.get("file", "").endswith(".c"):
            return e
    sys.exit("no C/C++ source entry in compile_commands.json")


def compiler_of(command):
    toks = shlex.split(command)
    if not toks:
        sys.exit("empty compile command")
    first = toks[0]
    if first.endswith("gcc") or first.endswith("g++"):
        return first
    sys.exit(f"unexpected compiler: {first}")


def find_chip(entries):
    for e in entries:
        m = re.search(r"-I\s+\S*?/components/soc/([A-Za-z0-9_]+)", e.get("command", ""))
        if m:
            return m.group(1)
    return None


def arch(compiler):
    base = os.path.basename(compiler)
    if "xtensa" in base:
        return "xtensa"
    if "riscv" in base:
        return "riscv32"
    sys.exit(f"unknown arch in compiler name: {base}")


def triple(arch, chip):
    if arch == "xtensa":
        return f"xtensa-{chip or 'esp32'}-unknown-elf"
    return "riscv32-unknown-elf"


def system_includes(command):
    """Run the real compile command with -E -v; return the toolchain dirs."""
    toks = shlex.split(command)
    explicit = set()
    out = []
    i = 0
    while i < len(toks):
        t = toks[i]
        if t in ("-I", "-isystem", "-iquote", "-idirafter"):
            explicit.add(os.path.normpath(toks[i + 1]))
            i += 2
            continue
        if t.startswith("-I") and len(t) > 2:
            explicit.add(os.path.normpath(t[2:]))
            i += 1
            continue
        if t == "-o" or t in ("-MF", "-MT", "-MQ", "-MJ"):
            i += 2
            continue
        if t in ("-c", "-MMD", "-MD", "-MP"):
            i += 1
            continue
        out.append(t)
        i += 1

    out = out + ["-E", "-v", "-o", "/dev/null"]
    p = subprocess.run(out, capture_output=True, text=True)
    err = p.stderr
    marker = "#include <...> search starts here:"
    end = "End of search list."
    a = err.find(marker)
    b = err.find(end, a)
    if a < 0 or b < 0:
        sys.exit(f"could not parse include dirs from the compiler:\n{err[-500:]}")
    dirs = []
    for line in err[a + len(marker):b].splitlines():
        d = line.strip()
        if d:
            d = os.path.normpath(d)
            if d not in explicit:
                dirs.append(d)
    return dirs


def arch_macros(compiler, command):
    toks = [t for t in shlex.split(command) if t.startswith("-m")]
    p = subprocess.run([compiler] + toks + ["-dM", "-E", "-x", "c", "/dev/null"],
                       capture_output=True, text=True)
    out = []
    for name in ("__XTENSA__", "__XTENSA_EL__"):
        m = re.search(rf"#define {name} (\d+)", p.stdout)
        if m:
            out.append(f"-D{name}={m.group(1)}")
    return out


def check_clangd():
    try:
        p = subprocess.run(["clangd", "--version"], capture_output=True, text=True)
    except FileNotFoundError:
        print("warning: clangd not on PATH; .clangd written but needs clangd >= 20",
              file=sys.stderr)
        return
    m = re.search(r"clangd version (\d+)", p.stdout + p.stderr)
    if m and int(m.group(1)) < 20:
        print(f"warning: clangd {m.group(1)} lacks Xtensa backend; install clangd >= 20",
              file=sys.stderr)


def main():
    project_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    entries = load_db(project_dir)
    entry = pick_entry(entries)
    command = entry["command"]
    gcc = compiler_of(command)
    a = arch(gcc)
    chip = find_chip(entries)

    includes = system_includes(command)
    if not includes:
        sys.exit("no system include dirs extracted")
    libc = "picolibc" if any("picolibc" in d for d in includes) else "newlib"

    macros = arch_macros(gcc, command)

    lines = ["CompileFlags:", "  CompilationDatabase: build", "  Add:"]
    lines.append(f"    - --target={triple(a, chip)}")
    lines.append("    - -nostdinc")
    for m in macros:
        lines.append(f"    - {m}")
    for d in includes:
        lines.append("    - -isystem")
        lines.append(f"    - {d}")
    lines.append("  Remove:")
    for f in REMOVE_FLAGS:
        lines.append(f"    - {f}")

    out_path = os.path.join(project_dir, ".clangd")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    check_clangd()
    print(f".clangd: arch={a} chip={chip or 'esp32'} libc={libc} "
          f"includes={len(includes)}")


if __name__ == "__main__":
    main()
