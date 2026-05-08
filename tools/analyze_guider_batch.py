#!/usr/bin/env python3
from __future__ import annotations

import json
import math
import sys
from collections import Counter
from pathlib import Path

import numpy as np
from astropy.io import fits
from PIL import Image, ImageDraw


def autoscale_to_u8(data: np.ndarray) -> np.ndarray:
    arr = np.asarray(data, dtype=np.float64)
    med = float(np.median(arr))
    mad = float(np.median(np.abs(arr - med)))
    sigma = max(1.0, mad * 1.4826)
    lo = med - 1.0 * sigma
    hi = med + 8.0 * sigma
    if hi <= lo:
        hi = lo + 1.0
    arr = np.clip((arr - lo) * 255.0 / (hi - lo), 0.0, 255.0)
    return arr.astype(np.uint8)


def local_peak(arr: np.ndarray, x: int, y: int, half: int = 2) -> float:
    h, w = arr.shape
    x0 = max(0, x - half)
    y0 = max(0, y - half)
    x1 = min(w, x + half + 1)
    y1 = min(h, y + half + 1)
    return float(np.max(arr[y0:y1, x0:x1]))


def detect_bright_peaks(data: np.ndarray, max_peaks: int = 12, threshold_sigma: float = 5.0, min_dist: int = 10):
    arr = np.asarray(data, dtype=np.float64)
    med = float(np.median(arr))
    mad = float(np.median(np.abs(arr - med)))
    sigma = max(1.0, mad * 1.4826)
    thr = med + threshold_sigma * sigma

    h, w = arr.shape
    candidates = []
    for y in range(2, h - 2):
        row = arr[y]
        for x in range(2, w - 2):
            v = row[x]
            if v < thr:
                continue
            patch = arr[y - 1 : y + 2, x - 1 : x + 2]
            if v < np.max(patch):
                continue
            candidates.append((float(v), x, y))

    candidates.sort(reverse=True)
    keep = []
    for v, x, y in candidates:
        if all((x - kx) ** 2 + (y - ky) ** 2 >= min_dist**2 for _, kx, ky in keep):
            keep.append((v, x, y))
        if len(keep) >= max_peaks:
            break

    return {"median": med, "sigma": sigma, "threshold": thr, "peaks": keep}


def draw_analysis_preview(data: np.ndarray, peaks, out_path: Path):
    base = autoscale_to_u8(data)
    img = Image.fromarray(base, mode="L").convert("RGB")
    draw = ImageDraw.Draw(img)

    for i, (v, x, y) in enumerate(peaks, start=1):
        r = 8
        draw.ellipse((x - r, y - r, x + r, y + r), outline=(255, 255, 0), width=1)
        draw.text((x + 10, y - 8), f"{i}:{int(v)}", fill=(255, 255, 0))

    img.save(out_path, quality=92)


def analyze_batch(batch_dir: Path, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    fits_files = sorted(batch_dir.glob("frame_*.fits"))
    if not fits_files:
        raise SystemExit(f"no frame_*.fits found in {batch_dir}")

    summary_frames = []
    top5_counter = Counter()

    for fp in fits_files:
        data = fits.getdata(fp)
        result = detect_bright_peaks(data)
        peaks = result["peaks"]

        preview_path = out_dir / f"{fp.stem}_peaks.jpg"
        draw_analysis_preview(data, peaks, preview_path)

        for _, x, y in peaks[:5]:
            top5_counter[(round(x / 10) * 10, round(y / 10) * 10)] += 1

        frame_info = {
            "frame": fp.stem,
            "fitsPath": str(fp),
            "previewPath": str(preview_path),
            "median": result["median"],
            "sigma": result["sigma"],
            "threshold": result["threshold"],
            "topPeaks": [
                {"rank": i + 1, "peakADU": v, "x": x, "y": y}
                for i, (v, x, y) in enumerate(peaks)
            ],
        }
        summary_frames.append(frame_info)

        print(
            f"[frame] {fp.stem} peaks={len(peaks)} "
            f"top1=({peaks[0][1]},{peaks[0][2]},{int(peaks[0][0])})" if peaks else f"[frame] {fp.stem} peaks=0"
        )

    summary = {
        "batchDir": str(batch_dir),
        "analysisDir": str(out_dir),
        "frameCount": len(summary_frames),
        "commonTop5Regions": [
            {"x10": x, "y10": y, "count": count}
            for (x, y), count in top5_counter.most_common(20)
        ],
        "frames": summary_frames,
    }
    summary_path = out_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"[done] summary={summary_path}")


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: analyze_guider_batch.py <batch_dir> [out_dir]")
    batch_dir = Path(sys.argv[1]).expanduser().resolve()
    out_dir = Path(sys.argv[2]).expanduser().resolve() if len(sys.argv) >= 3 else batch_dir / "py_analysis"
    analyze_batch(batch_dir, out_dir)


if __name__ == "__main__":
    main()
