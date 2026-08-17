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
import json
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
GUIDES = HERE.parent / "guides"
OUT = HERE / "hands-on.html"

# The link participants type or scan. Replace with the real one; the QR is generated from this
# exact string, so the two can never disagree.
HANDSON_URL = "https://github.com/REPLACE-ME"

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
    r = subprocess.run(["kpsewhich", name], capture_output=True, text=True)
    p = r.stdout.strip()
    if not p:
        die(f"font not found: {name}. It ships with TeX Live "
            "(libertine in collection-xetex, inconsolata in collection-fontsextra).")
    return pathlib.Path(p)


def font_faces():
    css = []
    for family, (fname, weight, style) in FONTS.items():
        uri = data_uri(find_font(fname).read_bytes(), "font/otf")
        css.append(f"@font-face{{font-family:'{family}';font-weight:{weight};"
                   f"font-style:{style};src:url({uri}) format('opentype');}}")
    return "\n".join(css)


def logo_uri(ident):
    """A logo as an inlined PNG. The PDFs are vector art for LaTeX; browsers need a raster.

    -r600 with pngalpha keeps the marks crisp when the deck is projected and preserves the
    transparency, so they sit on the slide background rather than in white boxes.
    """
    png = GUIDES / "logos" / f"{ident}.png"
    pdf = GUIDES / "logos" / f"{ident}.pdf"
    if png.exists():
        return data_uri(png.read_bytes(), "image/png")
    if not pdf.exists():
        return None
    if not shutil.which("gs"):
        die("ghostscript (gs) is needed to convert the PDF logos for the web")
    out = HERE / f".{ident}.tmp.png"
    subprocess.run(["gs", "-q", "-dNOPAUSE", "-dBATCH", "-sDEVICE=pngalpha", "-r600",
                    "-dUseCropBox", f"-sOutputFile={out}", str(pdf)], check=True)
    uri = data_uri(out.read_bytes(), "image/png")
    out.unlink()
    return uri


def qr_uri(url):
    """The QR, or None. Optional on purpose: a missing qrencode should cost you the code, not the
    deck -- the printed link is still on the slide."""
    if not shutil.which("qrencode"):
        print("  note: qrencode not installed, slide 2 will show the link without a QR code")
        return None
    out = HERE / ".qr.tmp.png"
    subprocess.run(["qrencode", "-o", str(out), "-s", "12", "-m", "1", "-l", "M", url], check=True)
    uri = data_uri(out.read_bytes(), "image/png")
    out.unlink()
    return uri


meta = json.loads((GUIDES / "tutorial.json").read_text())
logos = {i["id"]: logo_uri(i["id"]) for i in meta["institutions"]}


def people_row(key):
    out = []
    for p in meta.get(key, []):
        marks = "".join(f'<img class="logo" src="{logos[a["id"]]}" alt="">'
                        for a in p["affiliations"] if logos.get(a["id"]))
        out.append(f'<span class="person">{p["name"]}{marks}</span>')
    return "".join(out)


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
      <p class="url"><a href="{HANDSON_URL}">{HANDSON_URL}</a></p>
      {QR_IMG}
    </div>
    """,
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
.url a{font-family:'Inconsolata',monospace;font-size:42px;color:#4A7AAB;text-decoration:none;
       border-bottom:2px solid rgba(74,122,171,.3);padding-bottom:3px;word-break:break-all}
.qr{margin-top:36px;height:210px;image-rendering:pixelated}

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
if "--open" in sys.argv:
    subprocess.run(["open", str(OUT)])
