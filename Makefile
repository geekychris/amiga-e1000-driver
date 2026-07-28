# Placeholder Makefile — replaced in Phase 1 with real cross-compile.
#
# Intended flow:
#   make            — cross-compile virte1000.device via Docker
#   make test       — build tests/testopen.exe
#   make deploy     — push virte1000.device to DH1: via devbench MCP
#                     (needs a running amiga_mcp session on this Mac)
#   make clean      — nuke build/

.PHONY: all clean test deploy

all:
	@echo "Not implemented yet. See docs/PLAN.md — Phase 1."
	@exit 1

clean:
	rm -rf build

test:
	@echo "No tests yet. See docs/PLAN.md — Phase 1 will add tests/testopen.c."

deploy:
	@echo "No artifact to deploy yet."
