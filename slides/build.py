#!/usr/bin/env python3
"""Build the tutorial's slide deck as ONE self-contained HTML file.

    ./build.py            write hands-on.html
    ./build.py --open     ...and open it

Why a single file with everything inlined:

  * It has to work with no network. Conference wifi fails, and this is the deck that tells forty
    people where to go -- it cannot be the thing that is broken. No CDN, no reveal.js download, no
    external font or image request.
  * It has to look the same on someone else's laptop plugged into someone else's projector. The
    fonts are the guides' own (Biolinum, Libertine, Inconsolata, from TeX Live) embedded as data
    URIs, so there is no substitution to be surprised by five minutes before the session.
  * Logos ship as PDFs for LaTeX, which no browser will draw, so they are rasterised here with
    ghostscript and inlined too.

People, institutions and the conference name come from ../guides/tutorial.json -- the same file the
guides read, so a change to the roster lands in both without anyone remembering to sync them.

The deck itself is DECK, below: one entry per slide, plain HTML. Add slides there.
"""

import base64
import io
import json
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GUIDES = HERE.parent / "guides"
SHOTS = HERE / "images"
FONTDIR = HERE / "fonts"
OUT = HERE / "hands-on.html"

# Stand-in for a link that is not decided yet. Slides may ship with it; the build counts them and
# says so at the end, so an unreplaced link is loud rather than something you meet on the projector.
TODO_URL = "https://github.com/REPLACE-ME"

# Where people are sent to get onto a card. NOT the participants repo below -- getting onto a
# BlueField and reading a part's guide are two different destinations.
HANDSON_URL = TODO_URL

# The repo participants clone: the one admin/update_participants_repo_on_github.py writes. The
# per-part guide links derive from it, so a rename is one edit here.
PARTICIPANTS_REPO = "https://github.com/fchamicapereira/sigcomm26-tutorial-bluefield-participants"

TITLE = "Hands on with SmartNICs"

# Fonts, by TeX Live file name. Only the faces the deck actually uses -- every extra one is a
# megabyte of base64 for nothing.
FONTS = {
    "Biolinum":    ("LinBiolinum_R.otf", 400, "normal"),
    "BiolinumB":   ("LinBiolinum_RB.otf", 700, "normal"),
    "LibertineIt": ("LinLibertine_RI.otf", 400, "italic"),
    "Inconsolata": ("Inconsolatazi4-Regular.otf", 400, "normal"),
}


def die(msg):
    sys.exit(f"build.py: {msg}")


def data_uri(blob, mime):
    return f"data:{mime};base64," + base64.b64encode(blob).decode("ascii")


def find_font(name):
    """The font file: the deck's own fonts/ first, TeX Live's copy second.

    fonts/ holds the same four faces CTAN ships, ~1.3 MB, so the deck builds on a laptop with no
    TeX Live -- which is most laptops, and apt wants 1.35 GB of texlive-fonts-extra for exactly
    these files. kpsewhich stays as the fallback so a machine that already has them uses its own.
    """
    local = FONTDIR / name
    if local.exists():
        return local
    try:
        p = subprocess.run(["kpsewhich", name], capture_output=True, text=True).stdout.strip()
    except FileNotFoundError:
        p = ""
    if not p:
        die(f"font not found: {name}. Put it in {FONTDIR.name}/ (it is on CTAN) or install TeX Live "
            "(libertine in collection-xetex, inconsolata in collection-fontsextra).")
    return pathlib.Path(p)


def font_faces():
    css = []
    for family, (fname, weight, style) in FONTS.items():
        uri = data_uri(find_font(fname).read_bytes(), "font/otf")
        css.append(f"@font-face{{font-family:'{family}';font-weight:{weight};"
                   f"font-style:{style};src:url({uri}) format('opentype');}}")
    return "\n".join(css)


def rasterise(pdf, out):
    """Vector logo to transparent PNG at 600 dpi.

    600 dpi with an alpha channel keeps the marks crisp when projected and keeps them sitting on
    the slide background rather than in white boxes. Two backends because neither is everywhere:
    ghostscript is what a TeX-shaped machine already has, PyMuPDF is a pip install needing no root.
    """
    if shutil.which("gs"):
        subprocess.run(["gs", "-q", "-dNOPAUSE", "-dBATCH", "-sDEVICE=pngalpha", "-r600",
                        "-dUseCropBox", f"-sOutputFile={out}", str(pdf)], check=True)
        return
    try:
        import fitz  # PyMuPDF
    except ImportError:
        die("converting the PDF logos needs either ghostscript (gs) or PyMuPDF (pip install pymupdf)")
    with fitz.open(pdf) as doc:
        doc[0].get_pixmap(dpi=600, alpha=True).save(out)


def logo_uri(ident):
    """A logo as an inlined PNG. The PDFs are vector art for LaTeX; browsers need a raster."""
    png = GUIDES / "logos" / f"{ident}.png"
    pdf = GUIDES / "logos" / f"{ident}.pdf"
    if png.exists():
        return data_uri(png.read_bytes(), "image/png")
    if not pdf.exists():
        return None
    out = HERE / f".{ident}.tmp.png"
    rasterise(pdf, out)
    uri = data_uri(out.read_bytes(), "image/png")
    out.unlink()
    return uri


def qr_uri(url):
    """The QR, or None. Optional on purpose: a missing encoder should cost you the code, not the
    deck -- the printed link is still on the slide.

    qrencode if it is installed, segno (pure Python, no root) if not. Same parameters either way:
    12px modules, 1-module quiet zone, error level M.
    """
    if shutil.which("qrencode"):
        out = HERE / ".qr.tmp.png"
        subprocess.run(["qrencode", "-o", str(out), "-s", "12", "-m", "1", "-l", "M", url],
                       check=True)
        uri = data_uri(out.read_bytes(), "image/png")
        out.unlink()
        return uri
    try:
        import segno
    except ImportError:
        print("  note: no qrencode and no segno, the hands-on slide will show the link with no QR")
        return None
    buf = io.BytesIO()
    segno.make(url, error="m").save(buf, kind="png", scale=12, border=1)
    return data_uri(buf.getvalue(), "image/png")


def pretty(url):
    """A URL as it should be READ off a projector, not as it is typed. The scheme is noise at 30px
    and nobody types it anyway; the href keeps the real thing."""
    return url.removeprefix("https://").removeprefix("http://")


def image_uri(path):
    """A screenshot or figure, inlined. Same no-network rule as everything else here."""
    mime = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg"}.get(path.suffix.lower())
    if mime is None:
        die(f"unsupported image type: {path.name} (png or jpg)")
    return data_uri(path.read_bytes(), mime)


def shot(name):
    """The expected-outcome screenshot for a part, or a placeholder standing in its place.

    Missing on purpose is a normal state here: the outcome slides are written before the run that
    produces the screenshot. So a missing file prints a note and draws a labelled box rather than
    failing the build -- the deck stays presentable, and the gap is obvious on the slide instead of
    being a silent blank. Drop the file at the path below and rebuild; nothing else changes.
    """
    for ext in (".png", ".jpg", ".jpeg"):
        p = SHOTS / f"{name}{ext}"
        if p.exists():
            return f'<img class="shot" src="{image_uri(p)}" alt="">'
    print(f"  note: no screenshot yet for '{name}' -- drop one at "
          f"slides/images/{name}.png and rebuild")
    return (f'<div class="shot-missing"><span>screenshot<br>'
            f'<code>slides/images/{name}.png</code></span></div>')


meta = json.loads((GUIDES / "tutorial.json").read_text())
logos = {i["id"]: logo_uri(i["id"]) for i in meta["institutions"]}


def people_row(key):
    out = []
    for p in meta.get(key, []):
        marks = "".join(f'<img class="logo" src="{logos[a["id"]]}" alt="">'
                        for a in p["affiliations"] if logos.get(a["id"]))
        out.append(f'<span class="person">{p["name"]}{marks}</span>')
    return "".join(out)


def part(eyebrow, title, shot_name, guide=None, url=None):
    """The two slides every hands-on part gets: where its guide is, then what success looks like.

    One helper rather than three near-copies. The parts differ only in their name and their guide,
    and a deck where Part 2 has drifted a font size away from Part 1 reads as a mistake from the
    third row.

    guide= names a file at the root of the participants repo, and the slide shows the repo with the
    filename under it: the full blob URL is ~100 characters, which is not a thing anyone reads off
    a projector, while the repo alone fits on one line and is what they need to reach anyway.
    url= is for a part whose guide does not live there (or does not exist yet).
    """
    if guide:
        href, shown = f"{PARTICIPANTS_REPO}/blob/main/{guide}", pretty(PARTICIPANTS_REPO)
        note = f'<p class="filenote">&rarr; <code>{guide}</code></p>'
    else:
        href = shown = url
        note = ""
        shown = pretty(shown)
    return [
        f"""
    <div class="centred">
      <div class="eyebrow">{eyebrow} &mdash; in progress</div>
      <h2>{title}</h2>
      <p class="lead">The guide:</p>
      <p class="url"><a href="{href}">{shown}</a></p>
      {note}
    </div>
    """,
        # No h2 here on purpose. These screenshots are 3024px-wide terminals, and a 62px heading
        # costs the image ~110px of height -- which, at their 1.66 aspect, is ~180px of width. The
        # slide's whole job is "does your screen look like this?", so the caption folds into one
        # line and the picture gets everything else.
        f"""
    <div class="centred">
      <div class="shot-cap">{eyebrow} &middot; {title} &mdash; what you should see</div>
      {shot(shot_name)}
    </div>
    """,
    ]


QR = qr_uri(HANDSON_URL)
QR_IMG = f'<img class="qr" src="{QR}" alt="QR code for {HANDSON_URL}">' if QR else ""

# --- the deck --------------------------------------------------------------------------------
# One string per slide. Slide 1 mirrors the guides' header block on purpose: same order (what it
# is, which conference, which tutorial, who runs it) and the same logo-beside-the-name treatment,
# so the projector and the PDF on someone's laptop read as one set.
DECK = [
    f"""
    <div class="title-slide">
      <h1>{TITLE}</h1>
      <div class="conference">{meta["conference"]}</div>
      <hr>
      <div class="tutorial-title">{meta["tutorial-title"]}</div>
      <div class="people">{people_row("organizers")}</div>
    </div>
    """,
    f"""
    <div class="centred">
      <h2>Let&rsquo;s get you onto a BlueField&#8209;3</h2>
      <p class="lead">Follow these steps:</p>
      <p class="url"><a href="{HANDSON_URL}">{pretty(HANDSON_URL)}</a></p>
      {QR_IMG}
    </div>
    """,
    *part("Part 1", "DOCA Flow", "part1-doca-flow-outcome", guide="doca-flow.pdf"),
    *part("Part 2", "DOCA PCC", "part2-doca-pcc-outcome", guide="doca-pcc.pdf"),
    *part("Bonus", "Steering", "bonus-steering-outcome", url=TODO_URL),
]

CSS = """
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;background:#111;overflow:hidden}
/* A fixed 1280x720 stage scaled to the window: layout is then identical on every projector,
   which a percentage-based deck never quite is. */
#stage{position:absolute;top:50%;left:50%;width:1280px;height:720px;background:#fff;
       transform-origin:center center}
.slide{position:absolute;inset:0;display:none;padding:70px 90px}
.slide.on{display:flex;align-items:center;justify-content:center}
.title-slide,.centred{width:100%;text-align:center}

h1{font-family:'BiolinumB',sans-serif;font-weight:700;font-size:76px;letter-spacing:-.5px;
   color:#111;line-height:1.05}
.conference{font-family:'Biolinum',sans-serif;font-size:34px;color:#4A7AAB;margin-top:14px}
hr{border:0;border-top:1.5px solid #C8C8C8;width:58%;margin:34px auto}
.tutorial-title{font-family:'LibertineIt',Georgia,serif;font-style:italic;font-size:27px;
                color:#222}
.people{margin-top:44px;font-family:'Biolinum',sans-serif;font-size:25px;color:#111}
.person{white-space:nowrap;margin:0 15px;display:inline-block}
.logo{height:1.15em;vertical-align:-.19em;margin-left:.3em}

h2{font-family:'BiolinumB',sans-serif;font-weight:700;font-size:62px;color:#111;line-height:1.1}
.lead{font-family:'Biolinum',sans-serif;font-size:33px;color:#333;margin-top:40px}
.url{margin-top:22px}
/* 30px, not larger: the repo name is 68 characters, and at 42px it wrapped mid-string. One
   unbroken line people can read off and type beats a bigger one they have to reassemble. */
.url a{font-family:'Inconsolata',monospace;font-size:30px;color:#4A7AAB;text-decoration:none;
       border-bottom:2px solid rgba(74,122,171,.3);padding-bottom:3px;word-break:break-all}
.qr{margin-top:36px;height:210px;image-rendering:pixelated}

/* Part slides. The eyebrow carries which part you are in, so the h2 can be the plain name of the
   thing -- "DOCA Flow", not "Part 1: DOCA Flow" wrapped onto two lines. */
.eyebrow{font-family:'Biolinum',sans-serif;font-size:26px;letter-spacing:1.5px;
         text-transform:uppercase;color:#4A7AAB;margin-bottom:18px}
/* On an outcome slide the caption is not a kicker over a heading -- it IS the heading, the only
   text on the slide, so it is sized as one. 40px: the widest of the three captions then sits at
   81% of the line (46px would be 93%, too tight for a part with a longer name), and it costs the
   screenshot 31px of width against a 26px caption -- 3.6%, for a caption half again as large.
   Tracking drops to .5px because 1.5px is a small-text trick that looks loose this big. */
.shot-cap{font-family:'Biolinum',sans-serif;font-size:40px;letter-spacing:.5px;
          text-transform:uppercase;color:#4A7AAB;margin-bottom:18px}
.filenote{margin-top:16px;font-family:'Biolinum',sans-serif;font-size:24px;color:#555}
.filenote code{font-family:'Inconsolata',monospace;font-size:26px;color:#111}

/* 495px is the tallest that still clears the 40px caption and the 70px padding on a 720px stage --
   measured, not guessed. The screenshots are wide (1.66), so height is what binds: every pixel
   here is 1.66 of width. contain keeps the aspect ratio of whatever gets dropped in, and the
   placeholder below is the same height so the slide does not jump when the real one arrives. */
.shot{display:block;margin:14px auto 0;max-width:100%;max-height:495px;object-fit:contain;
      border:1px solid #DDD;border-radius:4px}
.shot-missing{display:flex;align-items:center;justify-content:center;width:70%;height:495px;
              margin:34px auto 0;border:2px dashed #CCC;border-radius:6px;
              font-family:'Biolinum',sans-serif;font-size:26px;color:#999;line-height:1.9}
.shot-missing code{font-family:'Inconsolata',monospace;font-size:23px;color:#777}

/* Deliberately dim and out of the way: useful while presenting, invisible from the third row. */
#num{position:fixed;right:14px;bottom:10px;font-family:'Biolinum',sans-serif;font-size:13px;
     color:#666}
"""

JS = """
const slides=[...document.querySelectorAll('.slide')];let i=0;
const num=document.getElementById('num');
function show(n){i=Math.max(0,Math.min(slides.length-1,n));
  slides.forEach((s,k)=>s.classList.toggle('on',k===i));
  num.textContent=(i+1)+' / '+slides.length;
  // The hash makes a slide linkable: hand someone "#2" and they land on it, and reloading
  // mid-talk puts you back where you were instead of at slide 1.
  history.replaceState(null,'','#'+(i+1));}
// Scale the fixed stage to whatever the projector gives us, keeping 16:9 and centring the letterbox.
function fit(){const s=Math.min(innerWidth/1280,innerHeight/720);
  document.getElementById('stage').style.transform=
    'translate(-50%,-50%) scale('+s+')';}
addEventListener('resize',fit);fit();
show((parseInt(location.hash.slice(1),10)||1)-1);
addEventListener('hashchange',()=>show((parseInt(location.hash.slice(1),10)||1)-1));
addEventListener('keydown',e=>{
  if(['ArrowRight','ArrowDown',' ','PageDown','n'].includes(e.key)){show(i+1);e.preventDefault();}
  else if(['ArrowLeft','ArrowUp','PageUp','p'].includes(e.key)){show(i-1);e.preventDefault();}
  else if(e.key==='Home'){show(0);}else if(e.key==='End'){show(slides.length-1);}
  else if(e.key==='f'){document.documentElement.requestFullscreen?.();}
  else if(e.key==='Escape'){document.exitFullscreen?.();}});
addEventListener('click',e=>{if(!e.target.closest('a'))show(i+1);});
"""

slides_html = "\n".join(f'<section class="slide">{s}</section>' for s in DECK)
html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{TITLE} — {meta["conference"]}</title>
<style>
{font_faces()}
{CSS}
</style>
</head>
<body>
<div id="stage">
{slides_html}
</div>
<div id="num"></div>
<script>
{JS}
</script>
</body>
</html>
"""

OUT.write_text(html)
kb = len(html.encode()) // 1024
print(f"  wrote {OUT.name}  ({len(DECK)} slides, {kb} KB, self-contained)")

pending = html.count(f'href="{TODO_URL}"')
if pending:
    print(f"  TODO: {pending} link(s) still point at {TODO_URL} -- and the QR encodes it as-is")
if "--open" in sys.argv:
    subprocess.run(["open", str(OUT)])
