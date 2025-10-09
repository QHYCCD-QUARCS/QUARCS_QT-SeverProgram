#!/usr/bin/env python3
"""
Generate a deterministic series of 16-bit star-field images for tracking tests.

Features
- Deterministic with a key (same key + params => identical sequence)
- Image size configurable
- In-focus stars rendered as 2D Gaussian PSF (by FWHM)
- Star density configurable (stars per megapixel)
- Global drift per frame and small per-star jitter
- Background and read-noise controls
- Output FITS (default) or PNG 16-bit

Example
  python3 gen_starfield.py \
    --width 1920 --height 1080 --frames 50 \
    --density 80 --fwhm 2.5 --drift-x 0.2 --drift-y 0.1 \
    --bg 200 --noise 3 --key "my-seed" --out-dir ./star_frames

Defaults are chosen for quick testing; all parameters have defaults.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
from typing import Tuple

import numpy as np

try:
    from astropy.io import fits  # type: ignore
    HAS_FITS = True
except Exception:
    HAS_FITS = False
    fits = None  # type: ignore

try:
    from PIL import Image  # type: ignore
    HAS_PIL = True
except Exception:
    HAS_PIL = False
    Image = None  # type: ignore


def key_to_seed(key: str) -> int:
    digest = hashlib.sha256(key.encode("utf-8")).hexdigest()
    # Use 64-bit seed from hex digest
    return int(digest[:16], 16) & 0xFFFFFFFFFFFFFFFF


def fwhm_to_sigma(fwhm: float) -> float:
    # FWHM = 2*sqrt(2*ln 2) * sigma ≈ 2.354820045 * sigma
    return max(0.1, fwhm / 2.354820045)


def render_stars(
    width: int,
    height: int,
    stars_xy: np.ndarray,  # shape (N, 2), float centers (x, y)
    fluxes: np.ndarray,    # shape (N,), peak amplitude in DN
    sigma: float,
) -> np.ndarray:
    """Render stars as 2D Gaussian PSF into a 16-bit image array (float64 internal)."""
    img = np.zeros((height, width), dtype=np.float64)
    # 4-sigma support window
    rad = max(2, int(np.ceil(4.0 * sigma)))
    yy, xx = np.mgrid[-rad:rad+1, -rad:rad+1]
    for (cx, cy), peak in zip(stars_xy, fluxes):
        ix = int(np.round(cx))
        iy = int(np.round(cy))
        x0 = ix - rad
        y0 = iy - rad
        x1 = ix + rad
        y1 = iy + rad
        if x1 < 0 or y1 < 0 or x0 >= width or y0 >= height:
            continue  # fully outside
        # window bounds intersection
        sx0 = max(0, x0)
        sy0 = max(0, y0)
        sx1 = min(width - 1, x1)
        sy1 = min(height - 1, y1)
        wx0 = sx0 - x0
        wy0 = sy0 - y0
        wx1 = wx0 + (sx1 - sx0)
        wy1 = wy0 + (sy1 - sy0)
        sub_xx = xx[wy0:wy1+1, wx0:wx1+1]
        sub_yy = yy[wy0:wy1+1, wx0:wx1+1]
        # continuous center offset
        dx = (sx0 + np.arange(sub_xx.shape[1])) - cx
        dy = (sy0 + np.arange(sub_xx.shape[0])) - cy
        DX, DY = np.meshgrid(dx, dy)
        psf = np.exp(-0.5 * (DX*DX + DY*DY) / (sigma*sigma)) * peak
        img[sy0:sy1+1, sx0:sx1+1] += psf
    return img


def generate_sequence(
    width: int,
    height: int,
    frames: int,
    density_mppm: float,
    fwhm: float,
    key: str,
    drift_per_frame: Tuple[float, float],
    jitter_std: float,
    bg: float,
    noise_std: float,
    amp_min: float,
    amp_max: float,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return (positions_txy, fluxes_tn, images_tyx) not materialized; we stream images.

    For memory efficiency we will stream frames; this function sets up the initial
    star field and returns stars arrays to be updated per frame.
    """
    rng = np.random.default_rng(key_to_seed(key))
    sigma = fwhm_to_sigma(fwhm)

    nstars = max(1, int(np.ceil(density_mppm * (width * height) / 1_000_000.0)))

    # Initial positions uniformly distributed, ensure inside edges considering PSF window
    margin = max(4, int(np.ceil(4 * sigma))) + 1
    xs = rng.uniform(margin, width - 1 - margin, size=nstars)
    ys = rng.uniform(margin, height - 1 - margin, size=nstars)
    flux = rng.uniform(amp_min, amp_max, size=nstars)

    positions = np.vstack([xs, ys]).T  # shape (N, 2)
    drift = np.array(drift_per_frame, dtype=np.float64)

    # Return state and a generator closure
    return positions, flux, drift


def save_fits(path: Path, data_u16: np.ndarray, header_dict: dict) -> None:
    if not HAS_FITS:
        raise RuntimeError("astropy is required to write FITS. Install astropy or use --format png")
    hdu = fits.PrimaryHDU(data=data_u16)
    hdr = hdu.header
    for k, v in header_dict.items():
        try:
            hdr[str(k)[:8].upper()] = v
        except Exception:
            pass
    hdul = fits.HDUList([hdu])
    hdul.writeto(str(path), overwrite=True)


def save_png(path: Path, data_u16: np.ndarray) -> None:
    if not HAS_PIL:
        raise RuntimeError("Pillow is required to write PNG. Install pillow or use --format fits")
    img = Image.fromarray(data_u16, mode="I;16")
    img.save(str(path))


def main() -> None:
    ap = argparse.ArgumentParser(description="Generate deterministic 16-bit star-field sequence")
    ap.add_argument("--width", type=int, default=640, help="image width in pixels")
    ap.add_argument("--height", type=int, default=480, help="image height in pixels")
    ap.add_argument("--frames", type=int, default=30, help="number of frames to generate")
    ap.add_argument("--density", type=float, default=80.0, help="stars per megapixel (Mpix)")
    ap.add_argument("--fwhm", type=float, default=2.5, help="star FWHM in pixels (in-focus ~2-3)")
    ap.add_argument("--drift-x", type=float, default=0.15, help="global drift in x per frame (pixels)")
    ap.add_argument("--drift-y", type=float, default=0.07, help="global drift in y per frame (pixels)")
    ap.add_argument("--jitter", type=float, default=0.05, help="per-star jitter std per frame (pixels)")
    ap.add_argument("--bg", type=float, default=200.0, help="background level (DN)")
    ap.add_argument("--noise", type=float, default=3.0, help="additive Gaussian noise std (DN)")
    ap.add_argument("--amp-min", type=float, default=800.0, help="min star peak (DN)")
    ap.add_argument("--amp-max", type=float, default=4000.0, help="max star peak (DN)")
    ap.add_argument("--key", type=str, default="quarcs-star-seed", help="deterministic key")
    ap.add_argument("--out-dir", type=str, default="./star_sequence", help="output directory")
    ap.add_argument("--format", choices=["fits", "png"], default="fits", help="output format")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    positions, flux, drift = generate_sequence(
        width=args.width,
        height=args.height,
        frames=args.frames,
        density_mppm=args.density,
        fwhm=args.fwhm,
        key=args.key,
        drift_per_frame=(args.drift_x, args.drift_y),
        jitter_std=args.jitter,
        bg=args.bg,
        noise_std=args.noise,
        amp_min=args.amp_min,
        amp_max=args.amp_max,
    )

    rng = np.random.default_rng(key_to_seed(args.key) ^ 0x9E3779B97F4A7C15)
    sigma = fwhm_to_sigma(args.fwhm)
    margin = max(4, int(np.ceil(4 * sigma))) + 1

    for t in range(args.frames):
        # Update positions with drift and jitter
        positions += drift
        positions += rng.normal(0.0, args.jitter, size=positions.shape)
        # Keep stars within margins (wrap-around)
        positions[:, 0] = np.mod(positions[:, 0] - margin, args.width - 2*margin) + margin
        positions[:, 1] = np.mod(positions[:, 1] - margin, args.height - 2*margin) + margin

        img = render_stars(args.width, args.height, positions, flux, sigma)
        # Add background and noise
        if args.noise > 0:
            img += rng.normal(0.0, args.noise, size=img.shape)
        img += args.bg
        # Clip to 16-bit
        img = np.clip(img, 0, 65535).astype(np.uint16)

        header = dict(
            WIDTH=args.width,
            HEIGHT=args.height,
            FRAMES=args.frames,
            FRAME=t,
            DENSITY=args.density,
            FWHM=args.fwhm,
            DRIFTX=args.drift_x,
            DRIFTY=args.drift_y,
            JITTER=args.jitter,
            BG=args.bg,
            NOISE=args.noise,
            KEY=args.key,
        )

        if args.format == "fits":
            out_path = out_dir / f"star_{t:04d}.fits"
            save_fits(out_path, img, header)
        else:
            out_path = out_dir / f"star_{t:04d}.png"
            save_png(out_path, img)

    print(f"Generated {args.frames} frames in {out_dir} ({args.format})")


if __name__ == "__main__":
    main()





