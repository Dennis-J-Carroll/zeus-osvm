#!/usr/bin/env python3
"""Generate examples/boot_splash.zasm from assets/logo_ascii.txt."""
import sys

ascii_path = sys.argv[1] if len(sys.argv) > 1 else "assets/logo_ascii.txt"
zasm_path = sys.argv[2] if len(sys.argv) > 2 else "examples/boot_splash.zasm"

with open(ascii_path, "r") as f:
    art = f.read()

# Escape backslashes and quotes for the assembler string literal.
escaped = art.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

src = f'''; boot_splash.zasm - print the Zeus_osVM ASCII logo on startup
; Generated from {ascii_path}

.data
splash:
    .asciz "{escaped}"

.code
start:
    PUSH splash
loop:
    DUP
    LOADBI
    DUP
    JZ done
    PRINTC
    PUSH 1
    ADD
    JMP loop
done:
    POP
    POP
    HALT
'''

with open(zasm_path, "w") as f:
    f.write(src)

print(f"Wrote {zasm_path}")
