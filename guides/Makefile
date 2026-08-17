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
#
# Also the fonts acmart uses, since the guides are set in ACM's stack: Linux Libertine/Biolinum
# and Inconsolata. Libertine comes with collection-xetex, so xelatex implies it, but Inconsolata
# is in collection-fontsextra only -- on a smaller TeX Live, `tlmgr install inconsolata`.
# template.tex says as much and stops with that line if the font is missing.

PANDOC ?= pandoc
ENGINE ?= xelatex

TEMPLATE := template.tex
METADATA := tutorial.json
# Turns the guides' "Try it yourself!" <details> disclosures into the boxed LaTeX environment
# template.tex defines. Pandoc drops raw HTML when writing LaTeX, so without this the summary lands
# in the PDF as an unstyled paragraph and nothing marks the exercise at all.
FILTER   := callouts.lua

MDS  := $(wildcard *.md)
PDFS := $(MDS:.md=.pdf)

# No --variable=logodir: pandoc runs the PDF engine with this directory as its working directory,
# so the template's relative logos/ path resolves as written. Passing an absolute one would also
# work right up until the repository sits somewhere with a space in its path, at which point the
# argument splits and every logo silently falls back to its placeholder.
PANDOC_FLAGS := \
	--template=$(TEMPLATE) \
	--metadata-file=$(METADATA) \
	--lua-filter=$(FILTER) \
	--pdf-engine=$(ENGINE) \
	--syntax-highlighting=tango

.PHONY: all clean
all: $(PDFS)

pcc-path-steering.pdf: pcc-path-steering-pdf.patch

# Every PDF depends on the shared header, metadata and filter, so correcting a room number or
# adding an organizer rebuilds all of them rather than just whichever guide was edited last.
#
# The output is captured so the "Missing character" check below can see it. That check exists
# because the failure it catches is otherwise invisible: xelatex DROPS a character its font cannot
# render, warns on stderr, and exits 0 -- so pandoc succeeds, make succeeds, and the PDF quietly
# has holes in it. It cost a review cycle once already, when ASCII-art diagrams drawn with
# box-drawing characters came out as blank gaps.
#
# Inconsolata is the culprit in practice: it is ACM's monospace font, so it sets every code block
# and inline code span, and it has no box-drawing, arrow or superscript glyphs. Draw diagrams in
# plain ASCII (- | + > v) and write `2^n` rather than a superscript. Prose is set in Linux
# Libertine, which is far better covered -- so a character that is fine in a sentence can still
# vanish inside `backticks`.
#
# Left as a warning rather than an error: a dropped glyph makes a PDF worse, not unusable, and
# failing the build would block someone mid-edit over a character they may have meant to change
# anyway. It is loud enough not to be missed.
%.pdf: %.md $(TEMPLATE) $(METADATA) $(FILTER)
	@echo "  PDF   $@"
	@tmp=; input="$<"; pdf_patch="$(basename $<)-pdf.patch"; \
	  if [ -f "$$pdf_patch" ]; then \
	    tmp="_pdf_$<"; cp "$<" "$$tmp"; \
	    if ! patch --silent "$$tmp" < "$$pdf_patch"; then rm -f "$$tmp"; exit 1; fi; input="$$tmp"; \
	  fi; \
	  out=$$($(PANDOC) "$$input" -o $@ $(PANDOC_FLAGS) 2>&1); status=$$?; \
	  printf '%s' "$$out" | grep . >&2 || true; \
	  if [ $$status -eq 0 ] && printf '%s' "$$out" | grep -q 'Missing character'; then \
	    echo "  WARN  $@: characters were DROPPED -- the PDF has gaps where they should be:" >&2; \
	    printf '%s' "$$out" | grep -o 'There is no .* (U+[0-9A-F]*) in font \[[^]/]*' \
	      | sed 's/^/          /' | sort -u >&2; \
	  fi; \
	  rm -f "$$tmp"; exit $$status

.PHONY: $(basename $(MDS))
$(basename $(MDS)): %: %.pdf

# Pre-publication check. Catches the two ways a SUCCESSFUL build still produces a bad PDF:
#
#   dropped glyphs   xelatex omits a character its font lacks (see the %.pdf rule above)
#   overfull lines   a line too wide for the text block, printed out into the margin
#
# The second is why this target exists at all: **pandoc does not report overfull boxes**, not even
# on stderr, so the %.pdf rule above cannot see them however carefully it inspects pandoc's output.
# One shipped unnoticed and was caught by a human reading the PDF. The only place they appear is
# xelatex's .log, so this runs pandoc to .tex and xelatex by hand to get at it.
#
# Not part of `all`, because it doubles the work and these are publication defects rather than
# build failures. Run it before handing a guide out, or before
# update_participants_repo_on_github.py.
#
# When it flags an overfull line, the fix is almost always in the Markdown, not the template: a long
# unbreakable `\texttt` token -- a 40-character DOCA identifier or an absolute path -- that TeX
# cannot hyphenate and will not break. Shorten it, or reword so it does not land mid-sentence.
# (\emergencystretch in template.tex helps ordinary prose, but cannot rescue those.)
.PHONY: check
check:
	@fail=0; \
	for md in $(MDS); do \
	  b=$${md%.md}; input=$$md; tmp=; pdf_patch=$$b-pdf.patch; \
	  if [ -f "$$pdf_patch" ]; then \
	    tmp=_pdf_$$md; cp "$$md" "$$tmp"; \
	    if ! patch --silent "$$tmp" < "$$pdf_patch"; then \
	      echo "  FAIL  $$b.pdf: cannot apply $$pdf_patch" >&2; rm -f "$$tmp"; fail=1; continue; \
	    fi; input=$$tmp; \
	  fi; \
	  $(PANDOC) $$input -o _check_$$b.tex $(PANDOC_FLAGS) 2>/dev/null; \
	  $(ENGINE) -interaction=batchmode _check_$$b.tex >/dev/null 2>&1 || true; \
	  ov=$$(grep -a -c '^Overfull \\hbox' _check_$$b.log 2>/dev/null || true); \
	  mc=$$(grep -a -c 'Missing character' _check_$$b.log 2>/dev/null || true); \
	  if [ "$${ov:-0}" -gt 0 ] || [ "$${mc:-0}" -gt 0 ]; then \
	    fail=1; \
	    echo "  FAIL  $$b.pdf: $${ov:-0} overfull line(s), $${mc:-0} dropped glyph(s)" >&2; \
	    grep -a '^Overfull \\hbox' _check_$$b.log 2>/dev/null | sed 's/^/          /' >&2 || true; \
	    grep -a -o 'There is no .* in font \[[^]/]*' _check_$$b.log 2>/dev/null \
	      | sort -u | sed 's/^/          /' >&2 || true; \
	  else \
	    echo "  ok    $$b.pdf"; \
	  fi; \
	  rm -f "$$tmp" _check_$$b.tex _check_$$b.log _check_$$b.aux _check_$$b.out _check_$$b.pdf; \
	done; \
	exit $$fail

clean:
	rm -f $(PDFS)
