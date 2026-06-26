#!/usr/bin/env python3
"""
flatfield_batch_test.py - 批量测试平场法 vs PHD2 星点检测
使用真实导星FITS文件，对比两种算法的检测结果。

用法：python3 flatfield_batch_test.py <fits_dir>
"""

import sys
import os
import glob
import time
import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree
from astropy.io import fits

def read_fits_16(path):
    """读取FITS文件为16位numpy数组"""
    with fits.open(path) as hdul:
        data = hdul[0].data
        if data.dtype != np.uint16:
            data = data.astype(np.uint16)
        return data

def median_blur_3x3(img):
    """3x3中值滤波"""
    return ndimage.median_filter(img, size=3)

def phd2_detect(img, snr_threshold=5.0):
    """
    简化版PHD2星点检测
    基于局部峰值检测 + SNR筛选
    """
    h, w = img.shape
    
    # 局部最大值检测 (5x5窗口)
    local_max = ndimage.maximum_filter(img, size=5) == img
    
    # 背景估计（使用截断均值）
    sorted_pixels = np.sort(img.ravel())
    bg = np.median(sorted_pixels[int(len(sorted_pixels)*0.1):int(len(sorted_pixels)*0.9)])
    noise = np.std(img[img < bg + 3*np.std(img)])
    if noise < 1:
        noise = 1.0
    
    candidates = []
    ys, xs = np.where(local_max)
    
    for x, y in zip(xs, ys):
        # 边缘剔除
        if x < 40 or y < 40 or x >= w-40 or y >= h-40:
            continue
        
        peak = img[y, x]
        snr = (peak - bg) / noise
        
        if snr >= snr_threshold:
            # 加权质心
            window = img[y-2:y+3, x-2:x+3].astype(np.float64)
            cy = np.sum(np.arange(5) * window.sum(axis=1)) / window.sum()
            cx = np.sum(np.arange(5) * window.sum(axis=0)) / window.sum()
            
            candidates.append({
                'x': x + cx - 2,
                'y': y + cy - 2,
                'snr': snr,
                'peak': peak
            })
    
    # 距离去重
    if len(candidates) > 1:
        points = np.array([[c['x'], c['y']] for c in candidates])
        tree = cKDTree(points)
        dists, _ = tree.query(points, k=2)
        keep = dists[:, 1] >= 5.0  # 最小间距5像素
        candidates = [c for c, k in zip(candidates, keep) if k]
    
    # 按SNR排序
    candidates.sort(key=lambda c: c['snr'], reverse=True)
    return candidates

def flatfield_detect(img, kernel_size=64, snr_threshold=5.0):
    """
    平场法星点检测
    1. 生成自平场（大核均值滤波）
    2. 扣除平场
    3. 峰值检测 + SNR筛选
    """
    h, w = img.shape
    
    # 生成自平场
    flat = ndimage.uniform_filter(img.astype(np.float64), size=kernel_size)
    
    # 扣除平场（保持偏移避免负值）
    offset = 2000.0
    subtracted = img.astype(np.float64) - flat + offset
    subtracted = np.clip(subtracted, 0, 65535).astype(np.uint16)
    
    # 在扣除平场后的图像上做峰值检测
    local_max = ndimage.maximum_filter(subtracted, size=5) == subtracted
    
    # 背景估计
    sorted_pixels = np.sort(subtracted.ravel())
    bg = np.median(sorted_pixels[int(len(sorted_pixels)*0.1):int(len(sorted_pixels)*0.9)])
    noise = np.std(subtracted[subtracted < bg + 3*np.std(subtracted)])
    if noise < 1:
        noise = 1.0
    
    candidates = []
    ys, xs = np.where(local_max)
    
    for x, y in zip(xs, ys):
        if x < 40 or y < 40 or x >= w-40 or y >= h-40:
            continue
        
        peak = subtracted[y, x]
        snr = (peak - bg) / noise
        
        if snr >= snr_threshold:
            window = subtracted[y-2:y+3, x-2:x+3].astype(np.float64)
            cy = np.sum(np.arange(5) * window.sum(axis=1)) / window.sum()
            cx = np.sum(np.arange(5) * window.sum(axis=0)) / window.sum()
            
            candidates.append({
                'x': x + cx - 2,
                'y': y + cy - 2,
                'snr': snr,
                'peak': peak
            })
    
    # 距离去重
    if len(candidates) > 1:
        points = np.array([[c['x'], c['y']] for c in candidates])
        tree = cKDTree(points)
        dists, _ = tree.query(points, k=2)
        keep = dists[:, 1] >= 5.0
        candidates = [c for c, k in zip(candidates, keep) if k]
    
    candidates.sort(key=lambda c: c['snr'], reverse=True)
    return candidates

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 flatfield_batch_test.py <fits_dir>")
        print("  Tests all .fits files in <fits_dir> with both PHD2 and FlatField detectors.")
        sys.exit(1)
    
    fits_dir = sys.argv[1]
    fits_files = sorted(glob.glob(os.path.join(fits_dir, '*.fits')) + 
                        glob.glob(os.path.join(fits_dir, '*.FITS')))
    
    print(f"Found {len(fits_files)} FITS files in: {fits_dir}")
    print('=' * 80)
    
    phd2_found = 0
    ff_found = 0
    phd2_total_ms = 0.0
    ff_total_ms = 0.0
    total = 0
    
    print(f"{'File':<45} {'PHD2':>8} {'FF':>8} {'PHD2_ms':>10} {'FF_ms':>10}")
    print('-' * 80)
    
    for fpath in fits_files:
        fname = os.path.basename(fpath)
        
        try:
            img = read_fits_16(fpath)
        except Exception as e:
            print(f"{fname:<45} SKIP (read error)")
            continue
        
        # 3x3中值滤波
        img_filtered = median_blur_3x3(img)
        
        # PHD2检测
        t0 = time.time()
        phd2_stars = phd2_detect(img_filtered, snr_threshold=5.0)
        phd2_ms = (time.time() - t0) * 1000
        phd2_total_ms += phd2_ms
        
        # 平场法检测
        t1 = time.time()
        ff_stars = flatfield_detect(img_filtered, kernel_size=64, snr_threshold=5.0)
        ff_ms = (time.time() - t1) * 1000
        ff_total_ms += ff_ms
        
        total += 1
        
        if phd2_stars:
            phd2_found += 1
        if ff_stars:
            ff_found += 1
        
        print(f"{fname:<45} {len(phd2_stars):>8} {len(ff_stars):>8} {phd2_ms:>10.1f} {ff_ms:>10.1f}")
        
        if phd2_stars:
            best = phd2_stars[0]
            print(f"  PHD2 best: x={best['x']:.1f} y={best['y']:.1f} SNR={best['snr']:.1f}")
        if ff_stars:
            best = ff_stars[0]
            print(f"  FF   best: x={best['x']:.1f} y={best['y']:.1f} SNR={best['snr']:.1f}")
    
    print('=' * 80)
    print(f"\n=== Summary ===")
    print(f"Total files: {total}")
    print(f"PHD2 detected: {phd2_found}/{total}")
    print(f"FlatField detected: {ff_found}/{total}")
    print(f"PHD2 avg time: {phd2_total_ms/total:.1f} ms" if total else "N/A")
    print(f"FlatField avg time: {ff_total_ms/total:.1f} ms" if total else "N/A")
    print(f"PHD2 total time: {phd2_total_ms:.1f} ms")
    print(f"FlatField total time: {ff_total_ms:.1f} ms")

if __name__ == '__main__':
    main()
