#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Store covers, README GIFs and a tour video, from the desktop preview's demo data.

  MOSAICO_PREVIEW_RECORD=1 GLIMDOCK_DIR=... python3 host/render.py \\
      --output build-host/media-raw --home-fixtures host/fixtures/swifty-demo
  python3 host/media.py build-host/media-raw

Needs Pillow, ffmpeg and (for the web settings page) Google Chrome or Chromium.
Everything shown comes from the preview's demo data, never from a real board:
a board's screens would show your own hosts, rooms and photos.

Writes store/covers/*.png (1920 x 1080, the Ideas store's covers),
docs/media/*.gif (the README's animations) and docs/media/glimdock-tour.mp4.
"""

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
FONT_SEMI = PROJECT / "components/mosaico_ui/assets/Inter-SemiBold.ttf"
FONT_REGULAR = PROJECT / "components/mosaico_ui/assets/Inter-Regular.ttf"
PAGE = PROJECT / "components/mosaico_shell/src/web_settings.html"
W, H = 1920, 1080
PANEL, RADIUS = 480, 80  # the panel and its corner radius, in its own pixels
FPS = 30  # the preview records a frame every 33 ms

CYAN, VIOLET, AMBER, GREEN, CORAL, BLUE = "#54D9F2", "#7A5BF5", "#F0B858", "#7AD6A0", "#FF8A70", "#5AB8FF"

# The web settings page as it looks with demo settings (no board needed).
DEMO_STATUS = """{"swifty":{"llm_url":"http://model-server:8080/v1","llm_model":"","llm_key":true,
"asr_url":"wss://speech.example/v1/asr/realtime","asr_model":"swifty-speech","asr_key":true,"asr_ca":false,
"search_key":true},"glimdock":{"ssid":"HomeWiFi","endpoint":"https://collector.example/api/v1/snapshot",
"password":true,"token":true,"setup_token":false,"ca":true},"home":{"paired":true,"hub":"http://hub.example:8080",
"name":"Living room","pairing":""},"files":{"shares":[{"name":"NAS","host":"nas.example","share":"media",
"user":"mosaico","path":"","password":true}],"photos":{"share":"NAS","path":"Mosaico Photos"}}}"""


def font(size, semi=True):
    return ImageFont.truetype(str(FONT_SEMI if semi else FONT_REGULAR), size)


def rgb(hex_color):
    return tuple(int(hex_color[i : i + 2], 16) for i in (1, 3, 5))


def rounded_mask(size, radius, scale=4):
    """An antialiased rounded-rectangle mask."""
    big = Image.new("L", (size[0] * scale, size[1] * scale), 0)
    ImageDraw.Draw(big).rounded_rectangle((0, 0, big.width - 1, big.height - 1), radius * scale, fill=255)
    return big.resize(size, Image.LANCZOS)


def screen(path):
    """A capture (PNG or PPM) as the panel shows it."""
    return Image.open(path).convert("RGB").resize((PANEL, PANEL))


def background(accent, glow_at):
    """Near black, with a soft glow of the accent behind the device."""
    bg = Image.new("RGB", (W, H), (6, 7, 10))
    glow = Image.new("L", (W, H), 0)
    x, y = glow_at
    ImageDraw.Draw(glow).ellipse((x - 520, y - 420, x + 520, y + 420), fill=120)
    glow = glow.filter(ImageFilter.GaussianBlur(160))
    bg = Image.composite(Image.new("RGB", (W, H), rgb(accent)), bg, glow.point(lambda v: v * 45 // 100))
    vignette = Image.new("L", (W, H), 0)
    ImageDraw.Draw(vignette).rectangle((0, 0, W, H), fill=255)
    ImageDraw.Draw(vignette).ellipse((-300, -400, W + 300, H + 400), fill=0)
    vignette = vignette.filter(ImageFilter.GaussianBlur(200)).point(lambda v: v * 70 // 100)
    return Image.composite(Image.new("RGB", (W, H), (0, 0, 0)), bg, vignette)


def device_geometry(size):
    """The bezel around a screen of size px, and the corner radius of both."""
    r = RADIUS * size / PANEL
    b = round(size * 0.05)
    return r, b


def draw_device(canvas, pos, size, shot=None):
    """A Mosaico: shadow, bezel, and the screen (shot) rounded into it. Without
    shot, the screen is left out (the video lays frames there, then the overlay)."""
    x, y = pos
    r, b = device_geometry(size)
    outer = (size + 2 * b, size + 2 * b)
    shadow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shadow).rounded_rectangle((x - b, y - b + 30, x + size + b, y + size + b + 40), r + b, fill=200)
    shadow = shadow.filter(ImageFilter.GaussianBlur(45))
    canvas.paste((0, 0, 0), (0, 0), shadow)
    body = Image.new("RGB", outer, (18, 18, 21))
    edge = Image.new("RGB", outer, (52, 52, 58))
    ring = rounded_mask(outer, r + b)
    inner_edge = Image.new("L", outer, 0)
    inner_edge.paste(rounded_mask((outer[0] - 4, outer[1] - 4), r + b - 2), (2, 2))
    canvas.paste(edge, (x - b, y - b), ring)
    canvas.paste(body, (x - b, y - b), inner_edge)
    if shot is not None:
        canvas.paste(shot.resize((size, size), Image.LANCZOS), (x, y), rounded_mask((size, size), r))
    return canvas


def bezel_overlay(pos, size):
    """For the video: the bezel and the corners outside the rounded screen,
    over frames laid in the square at pos."""
    x, y = pos
    r, b = device_geometry(size)
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    outer = (size + 2 * b, size + 2 * b)
    edge = Image.new("RGBA", outer, (52, 52, 58, 255))
    body = Image.new("RGBA", outer, (18, 18, 21, 255))
    over.paste(edge, (x - b, y - b), rounded_mask(outer, r + b))
    inner = Image.new("L", outer, 0)
    inner.paste(rounded_mask((outer[0] - 4, outer[1] - 4), r + b - 2), (2, 2))
    over.paste(body, (x - b, y - b), inner)
    hole = Image.new("L", (W, H), 0)
    hole.paste(rounded_mask((size, size), r), (x, y))
    alpha = ImageChops.subtract(over.getchannel("A"), hole)
    over.putalpha(alpha)
    return over


def wrap(draw, text, fnt, width):
    lines, line = [], ""
    for word in text.split():
        trial = (line + " " + word).strip()
        if draw.textlength(trial, font=fnt) <= width or not line:
            line = trial
        else:
            lines.append(line)
            line = word
    return lines + ([line] if line else [])


def draw_text(canvas, eyebrow, title, subtitle, accent, x=140, width=800, center_y=H // 2, title_px=84):
    d = ImageDraw.Draw(canvas)
    f_eye, f_title, f_sub = font(30), font(title_px), font(36 if title_px > 76 else 32, semi=False)
    line_h, sub_h = round(title_px * 1.17), 52 if title_px > 76 else 46
    title_lines = wrap(d, title, f_title, width)
    sub_lines = wrap(d, subtitle, f_sub, width)
    height = 46 + len(title_lines) * line_h + 26 + len(sub_lines) * sub_h
    y = center_y - height // 2
    d.text((x, y), eyebrow.upper(), font=f_eye, fill=rgb(accent))
    y += 46
    for line in title_lines:
        d.text((x, y), line, font=f_title, fill=(244, 244, 245))
        y += line_h
    y += 26
    for line in sub_lines:
        d.text((x, y), line, font=f_sub, fill=(166, 166, 173))
        y += sub_h


def browser_window(page_png, size=(980, 660)):
    """The web settings page in a plain browser window."""
    w, h = size
    win = Image.new("RGBA", size, (0, 0, 0, 0))
    frame = Image.new("RGB", size, (28, 28, 31))
    win.paste(frame, (0, 0), rounded_mask(size, 18))
    d = ImageDraw.Draw(win)
    for i, c in enumerate(("#FF5F57", "#FEBC2E", "#28C840")):
        d.ellipse((22 + i * 26, 18, 36 + i * 26, 32), fill=c)
    d.rounded_rectangle((160, 12, w - 160, 38), 8, fill=(44, 44, 48))
    d.text((w // 2, 25), "192.0.2.42", font=font(17, semi=False), fill=(166, 166, 173), anchor="mm")
    page = Image.open(page_png).convert("RGB")
    page = page.resize((w, round(page.height * w / page.width)), Image.LANCZOS).crop((0, 0, w, h - 50))
    win.paste(page, (0, 50))
    return win


def web_page_shot(out_png):
    """Screenshots the web settings page with demo settings in headless Chrome."""
    chrome = next(
        (
            p
            for p in (
                "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                shutil.which("chromium") or "",
                shutil.which("google-chrome") or "",
            )
            if p and Path(p).exists()
        ),
        None,
    )
    if not chrome:
        return False
    html = PAGE.read_text()
    mock = (
        "<script>window.fetch = async (path) => new Response(path === '/api/status' ? JSON.stringify("
        + DEMO_STATUS.replace("\n", "")
        + ') : \'{"reply":"ok"}\', {status: 200});</script>\n'
    )
    with tempfile.TemporaryDirectory() as tmp:
        page = Path(tmp) / "page.html"
        page.write_text(html.replace('<script>\n"use strict";', mock + '<script>\n"use strict";', 1))
        subprocess.run(
            [
                chrome,
                "--headless=new",
                "--hide-scrollbars",
                "--force-device-scale-factor=2",
                "--window-size=760,1100",
                f"--screenshot={out_png}",
                page.as_uri(),
            ],
            check=True,
            capture_output=True,
        )
    return True


# ---- covers ---------------------------------------------------------------------------------
def cover(out, eyebrow, title, subtitle, accent, shots, extra=None):
    """Text at the left, one device or two side by side at the right."""
    if len(shots) == 1:
        size, positions = 760, [(1060, (H - 760) // 2)]
        canvas = background(accent, (1440, H // 2))
        draw_text(canvas, eyebrow, title, subtitle, accent)
    else:
        size, positions = 480, [(804, 250), (1356, 350)]
        canvas = background(accent, (1330, H // 2))
        draw_text(canvas, eyebrow, title, subtitle, accent, x=110, width=600, title_px=72)
    for pos, shot in zip(positions, shots):
        draw_device(canvas, pos, size, screen(shot))
    if extra:
        extra(canvas)
    canvas.save(out, optimize=True)
    print("cover", out.name)


def covers(raw, out_dir, page_png):
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("*.png"):
        old.unlink()
    swifty_frame = sorted((raw / "swifty").glob("*.ppm"))[110]  # listening
    c = [
        (
            "01-launcher",
            "Glimdock for ESP-Mosaico",
            "Your desk, at a glance",
            "Homelab dashboard, smart home, a voice assistant, camera, files and tools in one launcher.",
            CYAN,
            [raw / "launcher-home-screen.png"],
        ),
        (
            "02-homelab",
            "Glimdock",
            "Your homelab, live",
            "Hosts, services, storage, sensors and alerts from your Glimdock collector.",
            CYAN,
            [raw / "glim-systems.png", raw / "glim-overview.png"],
        ),
        (
            "03-printers-agents",
            "Glimdock",
            "Prints and agents",
            "Follow a 3D print's temperatures and see what your AI agents are working on.",
            CYAN,
            [raw / "glim-printer-temps.png", raw / "glim-agents.png"],
        ),
        (
            "04-home",
            "Home",
            "Your home on the wall",
            "Rooms, lights, music, TV and plugs from a Swifty Home hub.",
            VIOLET,
            [raw / "home.png", raw / "home-lights.png"],
        ),
        (
            "05-swifty",
            "Swifty",
            "Hold the AI key and talk",
            "A voice assistant for your own model server. It searches the web, opens apps and controls the board.",
            BLUE,
            [swifty_frame],
        ),
        (
            "06-tools",
            "Tools",
            "Handy tools",
            "Torch, spirit level, compass, calculator and a USB webcam mode.",
            CORAL,
            [raw / "tools-level.png", raw / "tools-calc.png"],
        ),
    ]
    for name, eyebrow, title, subtitle, accent, shots in c:
        cover(out_dir / f"{name}.png", eyebrow, title, subtitle, accent, shots)
    # Web settings: the board's card and the page in a browser.
    canvas = background(GREEN, (1330, 560))
    draw_text(
        canvas,
        "Settings",
        "Set up from any browser",
        "Switch on Web settings, enter the code it shows, and edit URLs and keys. Keys never leave the board.",
        GREEN,
        x=110,
        width=600,
        title_px=72,
    )
    if page_png and page_png.exists():
        win = browser_window(page_png, (880, 600))
        x, y = 980, 100
        shadow = Image.new("L", (W, H), 0)
        ImageDraw.Draw(shadow).rounded_rectangle((x, y + 20, x + win.width, y + win.height + 30), 24, fill=170)
        canvas.paste((0, 0, 0), (0, 0), shadow.filter(ImageFilter.GaussianBlur(40)))
        canvas.paste(win, (x, y), win)
    draw_device(canvas, (800, 590), 430, screen(raw / "launcher-settings-web.png"))
    canvas.save(out_dir / "07-web-settings.png", optimize=True)
    print("cover 07-web-settings.png")


# ---- GIFs and the video ---------------------------------------------------------------------
CLIPS = [  # folder, eyebrow, title, subtitle, accent
    (
        "launcher",
        "Launcher",
        "One tap to everything",
        "Live widgets, every app a tap away, Settings with a web page for URLs and keys.",
        GREEN,
    ),
    (
        "glimdock",
        "Glimdock",
        "Your homelab, live",
        "Systems, a host's detail, a printer's temperatures, the clock and your agents.",
        CYAN,
    ),
    (
        "swifty",
        "Swifty",
        "Hold the AI key and talk",
        "It listens, thinks and answers, with a little bird that reacts.",
        BLUE,
    ),
    ("home", "Home", "Your home on the wall", "Rooms, lights and music from a Swifty Home hub.", VIOLET),
    ("level", "Tools", "A spirit level, and more", "Torch, compass, calculator and a USB webcam mode.", CORAL),
]


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", *map(str, args)], check=True)


def gif(frames, out, size=360, fps=20):
    """A README animation: the panel with its rounded corners, on black."""
    with tempfile.TemporaryDirectory() as tmp:
        corners = Path(tmp) / "corners.png"
        mask = rounded_mask((size, size), RADIUS * size / PANEL)
        over = Image.new("RGBA", (size, size), (0, 0, 0, 255))
        over.putalpha(ImageChops.invert(mask))
        over.save(corners)
        ffmpeg(
            "-framerate",
            FPS,
            "-i",
            frames / "%04d.ppm",
            "-i",
            corners,
            "-filter_complex",
            f"[0]fps={fps},scale={size}:{size}:flags=lanczos[s];[s][1]overlay,split[a][b];"
            "[a]palettegen=max_colors=192:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle",
            out,
        )
    print("gif", out.name, f"{out.stat().st_size / 1e6:.1f} MB")


def segment(tmp, index, frames, eyebrow, title, subtitle, accent):
    """One part of the tour: the clip in the device, its title beside it."""
    size, pos = 760, (1060, 160)
    bg = background(accent, (pos[0] + size // 2 + 120, H // 2))
    draw_text(bg, eyebrow, title, subtitle, accent)
    draw_device(bg, pos, size)
    bg_png, over_png = tmp / f"bg{index}.png", tmp / f"over{index}.png"
    bg.save(bg_png)
    bezel_overlay(pos, size).save(over_png)
    count = len(list(frames.glob("*.ppm")))
    seconds = count / FPS
    out = tmp / f"part{index}.mp4"
    ffmpeg(
        "-loop",
        1,
        "-framerate",
        FPS,
        "-i",
        bg_png,
        "-framerate",
        FPS,
        "-i",
        frames / "%04d.ppm",
        "-loop",
        1,
        "-framerate",
        FPS,
        "-i",
        over_png,
        "-filter_complex",
        f"[1]scale={size}:{size}:flags=lanczos[s];[0][s]overlay={pos[0]}:{pos[1]}:shortest=1[a];[a][2]overlay,"
        f"fade=t=in:st=0:d=0.35,fade=t=out:st={seconds - 0.35:.2f}:d=0.35,format=yuv420p",
        "-t",
        f"{seconds:.2f}",
        "-c:v",
        "libx264",
        "-preset",
        "slow",
        "-crf",
        20,
        "-r",
        FPS,
        out,
    )
    return out


def title_card(tmp, index, still, eyebrow, title, subtitle, accent, seconds=3.0):
    png = tmp / f"card{index}.png"
    size, pos = 760, (1060, 160)
    canvas = background(accent, (pos[0] + size // 2 + 120, H // 2))
    draw_text(canvas, eyebrow, title, subtitle, accent)
    draw_device(canvas, pos, size, screen(still))
    canvas.save(png)
    out = tmp / f"part{index}.mp4"
    ffmpeg(
        "-loop",
        1,
        "-framerate",
        FPS,
        "-i",
        png,
        "-vf",
        f"fade=t=in:st=0:d=0.4,fade=t=out:st={seconds - 0.4}:d=0.4,format=yuv420p",
        "-t",
        seconds,
        "-c:v",
        "libx264",
        "-preset",
        "slow",
        "-crf",
        20,
        "-r",
        FPS,
        out,
    )
    return out


def video(raw, out):
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        parts = [
            title_card(
                tmp,
                0,
                raw / "launcher-home-screen.png",
                "ESP-Mosaico",
                "Glimdock",
                "Homelab dashboard, smart home, a voice assistant and more, on one small square.",
                CYAN,
            )
        ]
        for i, (folder, eyebrow, title, subtitle, accent) in enumerate(CLIPS, 1):
            parts.append(segment(tmp, i, raw / folder, eyebrow, title, subtitle, accent))
        parts.append(
            title_card(
                tmp,
                99,
                raw / "launcher-settings-web.png",
                "Open source",
                "Make it yours",
                "github.com/gkostadinov/glimdock-mosaico",
                GREEN,
            )
        )
        listing = tmp / "parts.txt"
        listing.write_text("".join(f"file '{p}'\n" for p in parts))
        ffmpeg("-f", "concat", "-safe", 0, "-i", listing, "-c", "copy", "-movflags", "+faststart", out)
    print("video", out.name, f"{out.stat().st_size / 1e6:.1f} MB")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("raw", type=Path, help="render.py's output, recorded (MOSAICO_PREVIEW_RECORD=1)")
    parser.add_argument("--covers", type=Path, default=PROJECT / "store/covers")
    parser.add_argument("--media", type=Path, default=PROJECT / "docs/media")
    parser.add_argument("--skip-video", action="store_true")
    args = parser.parse_args()
    if not (args.raw / "swifty").is_dir():
        sys.exit("No recorded clips: run render.py with MOSAICO_PREVIEW_RECORD=1 first")
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is needed")
    args.media.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        page_png = Path(tmp) / "web-settings-page.png"
        if not web_page_shot(page_png):
            print("no Chrome: the web settings cover goes without the page", file=sys.stderr)
        covers(args.raw, args.covers, page_png)
    for folder, *_ in CLIPS:
        gif(args.raw / folder, args.media / f"{folder}.gif")
    if not args.skip_video:
        video(args.raw, args.media / "glimdock-tour.mp4")


if __name__ == "__main__":
    main()
