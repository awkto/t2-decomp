AS      := m68k-linux-gnu-as
LD      := m68k-linux-gnu-ld
ASFLAGS := -m68000 --register-prefix-optional
PY      := .venv/bin/python -I
BASEROM := rom/baserom.md
SHA1    := 85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56

all: build/t2.md
	@echo "$(SHA1)  build/t2.md" | sha1sum -c -

build/baserom.md: $(BASEROM)
	@mkdir -p build
	cp $(BASEROM) $@

build/t2.o: src/t2.s build/baserom.md
	$(AS) $(ASFLAGS) -I build -o $@ $<

build/t2.md: build/t2.o
	$(LD) -e 0 -Ttext=0 --oformat=binary -o $@ $<

disasm:
	$(PY) tools/py/disasm.py

# Regenerate src/t2.s + data/{functions,callgraph,xrefs}.txt (run after data/exec_starts.txt,
# data/symbols.txt or data/ram.txt change), then check every executed insn is in exactly one function.
functions: disasm
	$(PY) tools/py/symtab.py

# Runtime coverage report: docs/coverage.md
coverage: build/t2.o
	$(PY) tools/py/covreport.py --md docs/coverage.md

clean:
	rm -rf build

.PHONY: all disasm functions coverage clean
