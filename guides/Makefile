# Renders every .md in this directory to a same-named .pdf.
#
# Guides are Markdown; template.tex supplies the header and tutorial.json supplies everything the
# headers share, so a new guide is one new .md file and nothing else.
#
#   make                 build any .pdf whose source, template or metadata is newer
#   make -B              force a full rebuild
#   make tailscale       build one guide
#   make clean           remove the generated .pdf files
#
# Requires pandoc and xelatex (TeX Live). Both are what the repo already assumes for docs/.

PANDOC ?= pandoc
ENGINE ?= xelatex

TEMPLATE := template.tex
METADATA := tutorial.json

MDS  := $(wildcard *.md)
PDFS := $(MDS:.md=.pdf)

# No --variable=logodir: pandoc runs the PDF engine with this directory as its working directory,
# so the template's relative logos/ path resolves as written. Passing an absolute one would also
# work right up until the repository sits somewhere with a space in its path, at which point the
# argument splits and every logo silently falls back to its placeholder.
PANDOC_FLAGS := \
	--template=$(TEMPLATE) \
	--metadata-file=$(METADATA) \
	--pdf-engine=$(ENGINE) \
	--syntax-highlighting=tango

.PHONY: all clean
all: $(PDFS)

# Every PDF depends on the shared header and metadata, so correcting a room number or adding an
# organizer rebuilds all of them rather than just whichever guide was edited last.
%.pdf: %.md $(TEMPLATE) $(METADATA)
	@echo "  PDF   $@"
	@$(PANDOC) $< -o $@ $(PANDOC_FLAGS)

.PHONY: $(basename $(MDS))
$(basename $(MDS)): %: %.pdf

clean:
	rm -f $(PDFS)
