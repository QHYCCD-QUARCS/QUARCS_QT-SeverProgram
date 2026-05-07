#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from astropy.io import fits
from PIL import Image, ImageDraw


def normalize_web_path(path: Path, image_root: Path) -> str:
    clean_path = path.resolve()
    clean_root = image_root.resolve()
    rel = clean_path.relative_to(clean_root)
    return "/img/" + rel.as_posix()


def median3x3_u16(data: np.ndarray) -> np.ndarray:
    arr = np.asarray(data)
    if arr.ndim != 2:
        raise ValueError(f"expected 2D image, got shape {arr.shape}")
    padded = np.pad(arr, ((1, 1), (1, 1)), mode="edge")
    neighbors = [
        padded[dy:dy + arr.shape[0], dx:dx + arr.shape[1]]
        for dy in range(3)
        for dx in range(3)
    ]
    stacked = np.stack(neighbors, axis=0)
    return np.median(stacked, axis=0).astype(arr.dtype, copy=False)


def estimate_local_snr(data: np.ndarray,
                       x: float,
                       y: float,
                       source_radius: int = 4,
                       annulus_inner: int = 10,
                       annulus_outer: int = 18) -> float:
    arr = np.asarray(data, dtype=np.float64)
    h, w = arr.shape
    cx = int(round(x))
    cy = int(round(y))
    x0 = max(0, cx - annulus_outer)
    x1 = min(w - 1, cx + annulus_outer)
    y0 = max(0, cy - annulus_outer)
    y1 = min(h - 1, cy + annulus_outer)
    yy, xx = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    rr2 = (xx - x) ** 2 + (yy - y) ** 2

    source_mask = rr2 <= (source_radius ** 2)
    annulus_mask = (rr2 >= (annulus_inner ** 2)) & (rr2 <= (annulus_outer ** 2))
    if not np.any(source_mask) or not np.any(annulus_mask):
        return 0.0

    cut = arr[y0:y1 + 1, x0:x1 + 1]
    source_vals = cut[source_mask]
    annulus_vals = cut[annulus_mask]
    if source_vals.size == 0 or annulus_vals.size < 8:
        return 0.0

    bg_med = float(np.median(annulus_vals))
    bg_mad = float(np.median(np.abs(annulus_vals - bg_med)))
    bg_sigma = max(1.0, bg_mad * 1.4826)
    signal = float(np.sum(np.clip(source_vals - bg_med, 0.0, None)))
    noise = bg_sigma * max(1.0, np.sqrt(float(source_vals.size)))
    if noise <= 0.0:
        return 0.0
    return signal / noise


def write_median_fits(src_fits: Path, dst_fits: Path) -> Path:
    with fits.open(src_fits) as hdul:
        primary = hdul[0]
        medianed = median3x3_u16(primary.data)
        out_hdu = fits.PrimaryHDU(data=medianed, header=primary.header)
        fits.HDUList([out_hdu]).writeto(dst_fits, overwrite=True)
    return dst_fits


def draw_simplexy_overlay(raw_jpg: Path,
                          fits_path: Path,
                          rows: list[dict],
                          out_path: Path,
                          top_label_count: int,
                          top_draw_count: int) -> None:
    img = Image.open(raw_jpg).convert("RGB")
    with fits.open(fits_path) as hdul:
        fits_data = hdul[0].data
        fits_h, fits_w = fits_data.shape[:2]
    jpg_w, jpg_h = img.size
    scale_x = jpg_w / float(fits_w)
    scale_y = jpg_h / float(fits_h)
    draw = ImageDraw.Draw(img)
    for idx, row in enumerate(rows[:top_draw_count], start=1):
        # image2xy/simplexy reports source positions in the original FITS pixel space.
        # The guider JPG is a downscaled preview, so we map FITS-space coordinates
        # into the JPG preview coordinate system before drawing.
        x = float(row["x"]) * scale_x
        y = float(row["y"]) * scale_y
        snr = float(row.get("snr", 0.0))
        radius = 10 if idx <= top_label_count else 7
        color = (255, 255, 0) if idx <= top_label_count else (255, 140, 0)
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), outline=color, width=1)
        if idx <= top_label_count:
            draw.text((x + 10, y - 8), f"{idx}:S{snr:.1f}", fill=color)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path, quality=92)


def run_image2xy(image2xy: Path, fits_path: Path, xy_out: Path, sigma: float, psf_width: float, median_scale: int, saddle: float) -> str:
    cmd = [
        str(image2xy),
        "-O",
        "-o",
        str(xy_out),
        "-p",
        str(sigma),
        "-w",
        str(psf_width),
        "-s",
        str(median_scale),
        "-a",
        str(saddle),
        str(fits_path),
    ]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    return proc.stdout


def parse_xy_fits(xy_fits: Path) -> list[dict]:
    with fits.open(xy_fits) as hdul:
        data = hdul[1].data
        rows = [
            {
                "x": float(data["X"][i]),
                "y": float(data["Y"][i]),
                "flux": float(data["FLUX"][i]),
            }
            for i in range(len(data))
        ]
    return rows


def find_batch_summary(batch_dir: Path) -> Path:
    summary = batch_dir / "analysis" / "summary.json"
    if not summary.exists():
        raise FileNotFoundError(f"missing analyzer summary: {summary}")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Run astrometry image2xy/simplexy on a guider batch.")
    parser.add_argument("batch_dir", type=Path)
    parser.add_argument("--image2xy", type=Path, default=Path("/home/quarcs/workspace/astrometry-net-offline-20260402-arm64/native-build-snapshot/solver/image2xy"))
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--sigma", type=float, default=6.0)
    parser.add_argument("--psf-width", type=float, default=2.0)
    parser.add_argument("--median-scale", type=int, default=100)
    parser.add_argument("--saddle", type=float, default=5.0)
    parser.add_argument("--top-label-count", type=int, default=20)
    parser.add_argument("--top-draw-count", type=int, default=80)
    parser.add_argument("--tmp-dir", type=Path, default=Path("/tmp"))
    parser.add_argument("--use-median3x3", action="store_true", default=True)
    parser.add_argument("--no-use-median3x3", dest="use_median3x3", action="store_false")
    args = parser.parse_args()

    batch_dir = args.batch_dir.expanduser().resolve()
    out_dir = (args.out_dir.expanduser().resolve() if args.out_dir else (batch_dir / "simplexy_analysis"))
    out_dir.mkdir(parents=True, exist_ok=True)

    if not args.image2xy.exists():
        raise FileNotFoundError(f"image2xy not found: {args.image2xy}")

    analyzer_summary = json.loads(find_batch_summary(batch_dir).read_text())
    image_root = Path("/home/quarcs/images")
    frames_out = []

    for frame in analyzer_summary.get("frames", []):
        frame_name = frame["frame"]
        fits_path = Path(frame["fitsPath"])
        raw_jpg = Path(frame["rawJpgPath"])
        xy_out = args.tmp_dir / f"{frame_name}_simplexy.xy.fits"
        input_fits = fits_path
        if args.use_median3x3:
            input_fits = write_median_fits(fits_path, args.tmp_dir / f"{frame_name}_simplexy_median3x3.fits")
        log_text = run_image2xy(args.image2xy, input_fits, xy_out, args.sigma, args.psf_width, args.median_scale, args.saddle)

        if not xy_out.exists():
            print(f"[frame] {frame_name} failed: {log_text.strip()}", file=sys.stderr)
            frames_out.append(
                {
                    "frame": frame_name,
                    "count": 0,
                    "failed": True,
                    "log": log_text,
                }
            )
            continue

        with fits.open(input_fits) as hdul:
            snr_data = np.asarray(hdul[0].data)
        rows = parse_xy_fits(xy_out)
        for row in rows:
            row["snr"] = estimate_local_snr(snr_data, row["x"], row["y"])
        rows.sort(key=lambda row: (row["snr"], row["flux"]), reverse=True)
        overlay_path = out_dir / f"{frame_name}_simplexy.jpg"
        frame_json_path = out_dir / f"{frame_name}_simplexy.json"
        draw_simplexy_overlay(raw_jpg, fits_path, rows, overlay_path, args.top_label_count, args.top_draw_count)

        frame_info = {
            "frame": frame_name,
            "count": len(rows),
            "failed": False,
            "parameters": {
                "sigma": args.sigma,
                "psfWidth": args.psf_width,
                "medianScale": args.median_scale,
                "saddle": args.saddle,
                "useMedian3x3": args.use_median3x3,
            },
            "imagePath": str(overlay_path),
            "imageWebPath": normalize_web_path(overlay_path, image_root),
            "xyFitsPath": str(xy_out),
            "inputFitsPath": str(input_fits),
            "sortKey": "snr",
            "sourcesTop80": rows[:80],
            "top20": rows[:20],
            "log": log_text,
        }
        frame_json_path.write_text(json.dumps(frame_info, indent=2, ensure_ascii=False))
        frames_out.append(frame_info)
        print(f"[frame] {frame_name} count={len(rows)} top1={rows[0] if rows else None}")

    summary = {
        "batchDir": str(batch_dir),
        "analysisDir": str(out_dir),
        "frameCount": len(frames_out),
        "parameters": {
            "image2xy": str(args.image2xy),
            "sigma": args.sigma,
            "psfWidth": args.psf_width,
            "medianScale": args.median_scale,
            "saddle": args.saddle,
            "useMedian3x3": args.use_median3x3,
        },
        "frames": frames_out,
    }
    summary_path = out_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"[done] summary={summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
