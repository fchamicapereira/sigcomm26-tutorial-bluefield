# Renders every .dot in this directory to a same-named .png.
#
# 300 DPI keeps the figures sharp enough to paste straight into slides; override with e.g.
#   make DPI=96          (quick, screen-sized)
#
#   make                 render any .png whose .dot is newer
#   make -B              force a full re-render
#   make clean           remove the generated .png files

DPI ?= 300

DOTS := $(wildcard *.dot)
PNGS := $(DOTS:.dot=.png)

.PHONY: all clean
all: $(PNGS)

# `dot` warns "Arrow type ... unknown" on the edges that set an arrowhead alongside a compass
# port (the 100G cable, and the ported PF1 legs). The warning is benign — those edges render
# correctly — so filter it out, while still failing the build on any real dot error.
%.png: %.dot
	@echo "  DOT   $@"
	@out=$$(dot -Tpng -Gdpi=$(DPI) $< -o $@ 2>&1); status=$$?; \
	  printf '%s' "$$out" | grep -v 'Arrow type' | grep . >&2 || true; \
	  exit $$status

clean:
	rm -f $(PNGS)
