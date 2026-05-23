#!/usr/bin/env python3
"""
平场法星点检测 - Python 版本（用于与 C++ 实现性能对比）

用法：
  python3 flatfield_detect.py <fits_path> [options]
  python3 flatfield_detect.py --stdin < input.json

输入模式：
  1. FITS文件路径：直接读取FITS文件
  2. --stdin：从标准输入读取JSON {"data": [...], "rows": N, "cols": M}

输出：JSON 格式星点列表
  [{"x": 123.45, "y": 67.89, "snr": 15.2, "hfd": 3.1, "peakADU": 5000}, ...]
"""

import sys
import os
import json
import time
import numpy as np
from scipy.ndimage import uniform_filter, gaussian_filter, maximum_filter
from scipy.signal import medfilt

try:
    from astropy.io import fits
    HAS_ASTROPY = True
except ImportError:
    HAS_ASTROPY = False


def load_image(fits_path):
    """加载FITS文件，返回numpy数组"""
    if not HAS_ASTROPY:
        print("Error: astropy not installed", file=sys.stderr)
        sys.exit(1)
    with fits.open(fits_path) as hdul:
        data = hdul[0].data.astype(np.float32)
    return data


def generate_flat_field(image, kernel_size=64, method='uniform', sigma=0):
    """生成自平场"""
    if method == 'gaussian':
        s = sigma if sigma > 0 else kernel_size / 4.0
        return gaussian_filter(image, sigma=s, mode='nearest')
    elif method == 'median':
        ks = kernel_size if kernel_size % 2 == 1 else kernel_size + 1
        return medfilt(image, kernel_size=ks)
    else:
        return uniform_filter(image, size=kernel_size, mode='nearest')


def subtract_flat(image, flat):
    """扣除平场"""
    result = image - flat
    result[result < 0] = 0
    return result


def estimate_background_ring(image, x, y, inner_r=5, outer_r=15):
    """环形区域背景估计"""
    rows, cols = image.shape
    mask = np.zeros_like(image, dtype=bool)
    yy, xx = np.ogrid[:rows, :cols]
    r2 = (xx - x)**2 + (yy - y)**2
    mask = (r2 >= inner_r**2) & (r2 <= outer_r**2)
    vals = image[mask]
    if len(vals) == 0:
        return 0.0, 1.0
    return float(np.mean(vals)), float(np.std(vals)) if np.std(vals) > 0 else 1.0


def compute_hfd(image, cx, cy, half_size=8):
    """HFD计算（按距离排序）"""
    rows, cols = image.shape
    icx, icy = int(round(cx)), int(round(cy))
    y_min = max(0, icy - half_size)
    y_max = min(rows - 1, icy + half_size)
    x_min = max(0, icx - half_size)
    x_max = min(cols - 1, icx + half_size)

    roi = image[y_min:y_max+1, x_min:x_max+1]
    yy, xx = np.ogrid[0:roi.shape[0], 0:roi.shape[1]]
    dx = xx - (icx - x_min)
    dy = yy - (icy - y_min)
    dists = np.sqrt(dx**2 + dy**2)

    pixels = roi.flatten()
    distances = dists.flatten()

    idx = np.argsort(distances)
    sorted_pixels = pixels[idx]
    sorted_dists = distances[idx]

    total_flux = np.sum(sorted_pixels)
    half_flux = total_flux * 0.5
    cum_flux = np.cumsum(sorted_pixels)

    pos = np.searchsorted(cum_flux, half_flux)
    if pos >= len(sorted_dists):
        pos = len(sorted_dists) - 1
    return float(sorted_dists[pos] * 2)


def weighted_centroid(image, approx_x, approx_y, half_size=5, k_sigma=2.0):
    """加权质心"""
    icx, icy = int(round(approx_x)), int(round(approx_y))
    rows, cols = image.shape
    x0 = max(0, icx - half_size)
    y0 = max(0, icy - half_size)
    x1 = min(cols - 1, icx + half_size)
    y1 = min(rows - 1, icy + half_size)

    roi = image[y0:y1+1, x0:x1+1]
    mean = np.mean(roi)
    std = np.std(roi)
    if std < 1.0:
        std = 1.0
    thr = mean + k_sigma * std

    mask = roi > thr
    if not np.any(mask):
        return approx_x, approx_y

    weights = roi - mean
    weights[weights < 0] = 0
    yy, xx = np.ogrid[0:roi.shape[0], 0:roi.shape[1]]
    sum_w = np.sum(weights)
    if sum_w < 1e-10:
        return approx_x, approx_y
    cx = (x0 + xx) * weights
    cy = (y0 + yy) * weights
    return float(np.sum(cx) / sum_w), float(np.sum(cy) / sum_w)


def detect_stars(image, kernel_size=64, snr_threshold=5.0, min_hfd=1.5,
                 max_hfd=12.0, min_separation=5, edge_margin=40.0,
                 method='uniform', sigma=0, centroid_half_size=5):
    """平场法星点检测主函数"""
    t0 = time.time()

    # Step 1: 生成平场
    flat = generate_flat_field(image, kernel_size, method, sigma)

    # Step 2: 扣除平场
    flat_sub = subtract_flat(image, flat)

    # Step 3: 峰值检测
    peak_window = max(5, min_separation * 2 + 1)
    if peak_window % 2 == 0:
        peak_window += 1
    local_max = maximum_filter(flat_sub, size=peak_window, mode='nearest')
    peak_mask = np.abs(flat_sub - local_max) < 0.01

    # Step 4: 收集候选星
    candidates = []
    ys, xs = np.where(peak_mask)

    for x, y in zip(xs, ys):
        bg_mean, bg_std = estimate_background_ring(image, x, y)
        signal = flat_sub[y, x] - bg_mean
        snr = signal / bg_std if bg_std > 0 else 0.0

        if snr < snr_threshold * 0.5:
            continue

        # 质心精化
        cx, cy = weighted_centroid(image, x, y, centroid_half_size)

        # 峰值ADU
        half = centroid_half_size
        x0 = max(0, int(round(cx)) - half)
        y0 = max(0, int(round(cy)) - half)
        x1 = min(image.shape[1] - 1, int(round(cx)) + half)
        y1 = min(image.shape[0] - 1, int(round(cy)) + half)
        peak_adu = float(np.max(image[y0:y1+1, x0:x1+1]))

        # HFD
        hfd = compute_hfd(image, cx, cy)

        # 边缘距离
        edge_dist = min(cx, cy, image.shape[1] - 1 - cx, image.shape[0] - 1 - cy)

        candidates.append({
            'x': cx, 'y': cy, 'snr': snr, 'hfd': hfd,
            'peakADU': peak_adu, 'edgeDistPx': edge_dist
        })

    # Step 5: 距离去重（按SNR排序，贪心）
    candidates.sort(key=lambda c: c['snr'], reverse=True)
    deduped = []
    for c in candidates:
        too_close = False
        for d in deduped:
            dist = ((c['x'] - d['x'])**2 + (c['y'] - d['y'])**2)**0.5
            if dist < min_separation:
                too_close = True
                break
        if not too_close:
            deduped.append(c)
    candidates = deduped

    # Step 6: 筛选
    rows, cols = image.shape
    sat_threshold = 65535.0 * 0.9
    final = []
    for c in candidates:
        if c['edgeDistPx'] < edge_margin:
            continue
        if c['peakADU'] > sat_threshold:
            continue
        if c['hfd'] < min_hfd or c['hfd'] > max_hfd:
            continue
        if c['snr'] < snr_threshold:
            continue
        final.append(c)

    elapsed = time.time() - t0
    return final, elapsed


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Flat-field star detection')
    parser.add_argument('fits_path', nargs='?', help='FITS file path')
    parser.add_argument('--stdin', action='store_true', help='Read from stdin JSON')
    parser.add_argument('--kernel-size', type=int, default=64)
    parser.add_argument('--snr-threshold', type=float, default=5.0)
    parser.add_argument('--min-hfd', type=float, default=1.5)
    parser.add_argument('--max-hfd', type=float, default=12.0)
    parser.add_argument('--min-separation', type=int, default=5)
    parser.add_argument('--edge-margin', type=float, default=40.0)
    parser.add_argument('--method', choices=['uniform', 'gaussian', 'median'], default='uniform')
    parser.add_argument('--sigma', type=float, default=0)
    parser.add_argument('--centroid-half-size', type=int, default=5)
    args = parser.parse_args()

    if args.stdin:
        input_data = json.loads(sys.stdin.read())
        image = np.array(input_data['data'], dtype=np.float32).reshape(
            input_data['rows'], input_data['cols'])
    elif args.fits_path:
        image = load_image(args.fits_path)
    else:
        print("Error: provide FITS path or --stdin", file=sys.stderr)
        sys.exit(1)

    stars, elapsed = detect_stars(
        image,
        kernel_size=args.kernel_size,
        snr_threshold=args.snr_threshold,
        min_hfd=args.min_hfd,
        max_hfd=args.max_hfd,
        min_separation=args.min_separation,
        edge_margin=args.edge_margin,
        method=args.method,
        sigma=args.sigma,
        centroid_half_size=args.centroid_half_size
    )

    output = {
        'stars': stars,
        'count': len(stars),
        'elapsed_ms': round(elapsed * 1000, 2),
        'image_shape': list(image.shape),
        'params': {
            'kernel_size': args.kernel_size,
            'snr_threshold': args.snr_threshold,
            'method': args.method
        }
    }

    print(json.dumps(output, ensure_ascii=False))


if __name__ == '__main__':
    main()
