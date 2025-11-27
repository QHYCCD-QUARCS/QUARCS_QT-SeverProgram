#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
snr_focus_rank.py

根据星点信噪比评估粗对焦图像的“接近准焦程度”：
1. 对每张 FITS 图像：
   - 估计背景和噪声 sigma
   - 连通域检测星点区域
   - 按 SNR 排序星点，取前 N 个（默认 100）
   - 计算所有星点的平均 SNR 以及前 N 个星点的平均 SNR
   - 打印前 N 个星点的 SNR 列表
2. 最后对所有图像按“前 N 个星点平均 SNR”排序，找出最接近准焦的图像

用法示例：
    python3 snr_focus_rank.py *.fits
    python3 snr_focus_rank.py --max-stars 100 --snr-threshold 5 --min-area 4 --debug --debug-dir debug_out *.fits
"""

import os
import argparse
import numpy as np

from astropy.io import fits
from astropy.stats import sigma_clipped_stats
from scipy.ndimage import (
    label,
    find_objects,
    median_filter,
    binary_closing,
    generate_binary_structure,
    binary_erosion,
)

import warnings

# 固定参数配置（根据原来命令行示例写死）
DETECT_SIGMA = 3.0       # 原脚本默认值
SNR_THRESHOLD = 6.0      # --snr-threshold 1
MIN_AREA = 4            # --min-area 4
BORDER_MARGIN = 10       # 原脚本默认值
MAX_STARS = 100          # --max-stars 100
MEDIAN_SIZE = 0          # 不做中值滤波
DOWNSAMPLE = 5           # --downsample 5

# 圆形度（circularity）筛选：4πA / P^2，1 为完美圆，越小越“不圆”
# 设置成稍微宽松的阈值，避免误杀正常星点
CIRCULARITY_MIN = 0.4

def read_fits_data(filename):
    """读取 FITS 文件数据，返回 2D 浮点数组。"""
    # 关键修正：memmap=False，避免 BZERO/BSCALE/BLANK 报错
    with fits.open(filename, memmap=False) as hdul:
        # 优先使用第一个有数据的 HDU
        data = None
        for hdu in hdul:
            if hasattr(hdu, "data") and hdu.data is not None:
                data = hdu.data
                break
        if data is None:
            raise RuntimeError(f"{filename}: 没有找到数据数组")

        # 如果是多维，尽量取最后两维作为图像
        if data.ndim > 2:
            data = data.reshape(data.shape[-2], data.shape[-1])

        return np.array(data, dtype=np.float64)


def preprocess_image(image,
                     median_size=0,
                     downsample=1):
    """
    对图像做可选的中值滤波和像素合并 down sample 预处理。

    参数：
        median_size: 中值滤波窗口大小（应为奇数）。<=1 时不进行中值滤波。
        downsample:  像素合并因子（>1 时按块平均进行降采样）。
    """
    # 中值滤波
    if median_size is not None and median_size > 1:
        image = median_filter(image, size=int(median_size))

    # 像素合并 down sample（块平均）
    if downsample is not None and downsample > 1:
        f = int(downsample)
        h, w = image.shape
        # 只保留能整除的部分，避免 reshape 出错
        h2 = (h // f) * f
        w2 = (w // f) * f
        if h2 > 0 and w2 > 0:
            img_c = image[:h2, :w2]
            # 形状重排为 (h2/f, f, w2/f, f)，再对小块做平均
            img_c = img_c.reshape(h2 // f, f, w2 // f, f).mean(axis=(1, 3))
            image = img_c

    return image


def detect_stars(image,
                 detect_sigma=3.0,
                 snr_threshold=5.0,
                 min_area=4,
                 border_margin=10,
                 max_stars=100):
    """
    在图像中检测星点并计算信噪比。

    返回：
        stars_all:   所有通过筛选的星点列表（每个是 dict）
        stars_top:   按 SNR 排序后的前 max_stars 星点列表
        stats:       背景 median, 噪声 sigma
    """
    # 估计背景和噪声
    mean, median, sigma = sigma_clipped_stats(image, sigma=3.0, maxiters=5)
    if sigma <= 0:
        sigma = 1e-6  # 防止除零

    # 阈值检测：像素 > median + detect_sigma * sigma
    threshold = median + detect_sigma * sigma
    mask = image > threshold

    # 形态学闭运算：先膨胀再腐蚀，填补星点内部的小空洞/断口，
    # 避免同一个大星点被分割成多个独立连通域
    structure = generate_binary_structure(2, 1)  # 3x3 十字形结构元素
    mask = binary_closing(mask, structure=structure, iterations=1)

    # 连通域标记
    labels, num = label(mask)
    if num == 0:
        return [], [], (median, sigma), mask, labels

    slices = find_objects(labels)

    h, w = image.shape
    stars = []

    for i in range(num):
        slc = slices[i]
        if slc is None:
            continue

        subimg = image[slc]
        sublab = labels[slc]

        comp_mask = (sublab == (i + 1))
        area = int(comp_mask.sum())
        if area < min_area:
            continue

        # 边缘剔除：避免落在边缘的“半个星点”
        y0, y1 = slc[0].start, slc[0].stop
        x0, x1 = slc[1].start, slc[1].stop
        if (y0 < border_margin or x0 < border_margin or
                y1 > h - border_margin or x1 > w - border_margin):
            continue

        # 圆形度检测：用 4πA / P^2 过滤重叠/拖尾/不规则星点
        # 近似周长 P = 边界像素个数（用一次腐蚀得到边界）
        eroded = binary_erosion(comp_mask, structure=np.ones((3, 3), dtype=bool))
        border = comp_mask & (~eroded)
        perimeter = int(border.sum())
        if perimeter <= 0:
            circularity = 0.0
        else:
            circularity = float(4.0 * np.pi * area / (perimeter ** 2))

        if circularity < CIRCULARITY_MIN:
            # 圆形度太低，可能是重叠星点或形状异常，丢弃
            continue

        values = subimg[comp_mask]
        peak_value = float(values.max())
        # 像素层面的峰值 SNR
        peak_snr = (peak_value - median) / sigma if sigma > 0 else 0.0

        if peak_snr < snr_threshold:
            # 峰值 SNR 太低，认为是噪点
            continue

        # 星点总信号（减去背景）
        signal = (values - median).sum()
        # 噪声估计：包含泊松噪声 + 背景噪声
        noise_var = signal + area * (sigma ** 2)
        if noise_var <= 0:
            snr_total = 0.0
        else:
            snr_total = signal / np.sqrt(noise_var)

        # 质心（简单平均）
        coords = np.argwhere(comp_mask)
        # coords 中是 (y, x) 相对 slc 的坐标
        cy_rel = coords[:, 0].mean()
        cx_rel = coords[:, 1].mean()
        cy = cy_rel + y0
        cx = cx_rel + x0

        # 计算 HFR（Half-Flux Radius，半通量半径，单位：像素）
        # 使用该星点连通域上的像素进行径向累积，得到经典的“半通量半径”
        # coords 中是 comp_mask 内像素的 (y, x)，相对当前切片 slc
        if coords.size == 0:
            hfr = 0.0
        else:
            # 相对质心的半径（允许亚像素）
            rel_y = coords[:, 0] - cy_rel
            rel_x = coords[:, 1] - cx_rel
            r = np.sqrt(rel_y ** 2 + rel_x ** 2)

            # 净信号：减去全局背景 median，并截去负值
            flux = values.astype(np.float64) - median
            flux[flux < 0] = 0.0

            if np.all(flux <= 0):
                hfr = 0.0
            else:
                order = np.argsort(r)
                r_sorted = r[order]
                flux_sorted = flux[order]
                cumsum = np.cumsum(flux_sorted)
                half_flux = 0.5 * cumsum[-1]

                # 在线性插值层面求解 half-flux 对应的半径，获得亚像素级 HFR
                idx_hfr = np.searchsorted(cumsum, half_flux)
                if idx_hfr == 0:
                    hfr = float(r_sorted[0])
                elif idx_hfr >= len(r_sorted):
                    hfr = float(r_sorted[-1])
                else:
                    r1, r2 = r_sorted[idx_hfr - 1], r_sorted[idx_hfr]
                    f1, f2 = cumsum[idx_hfr - 1], cumsum[idx_hfr]
                    if f2 <= f1:
                        hfr = float(r2)
                    else:
                        t = (half_flux - f1) / (f2 - f1)
                        hfr = float(r1 + t * (r2 - r1))

        # 过滤掉 HFR 过小的伪星点，避免极小连通域或异常形状干扰统计
        # 注意：同时会排除 hfr == 0.0 的无效结果
        if hfr < 0.5:
            continue

        # 说明：
        # - peak_snr 反映“单像素信噪比/锐利程度”，适合作为对焦好坏的评价指标；
        # - snr_total 是经典的总光子计数 SNR，星点越大、总通量越大就越高，
        #   这会导致“大而虚的星点 SNR 反而更高”，不利于对焦排序，因此只做参考。
        star = {
            "label": i + 1,
            "area": area,
            "peak_value": peak_value,
            "peak_snr": float(peak_snr),      # 用于对焦评价与排序 & 过滤
            "snr_total": float(snr_total),    # 物理意义上的总 SNR，仅供参考
            "y0": int(y0),
            "y1": int(y1),
            "x0": int(x0),
            "x1": int(x1),
            "cy": float(cy),
            "cx": float(cx),
            "hfr": float(hfr),                # 半能量半径（像素）
            "circularity": float(circularity) # 圆形度
        }
        stars.append(star)

    # 对焦评价原本会按 “峰值 SNR” 排序并截取前 max_stars 个，
    # 但当前脚本只需要 median_HFR，因此直接返回所有星点
    return stars, (median, sigma), mask, labels


def process_file(filename):
    """处理单个 FITS 文件，计算并输出 median_HFR。

    返回:
        median_hfr
    """
    print(f"处理文件：{filename}")

    try:
        image = read_fits_data(filename)
    except Exception as e:
        print(f"{filename}: 读取失败: {e}")
        return None

    # 原始尺寸
    h0, w0 = image.shape
    print(f"原始图像尺寸：{w0} x {h0}")

    # 预处理：中值滤波 + 像素合并 down sample
    image = preprocess_image(
        image,
        median_size=MEDIAN_SIZE,
        downsample=DOWNSAMPLE,
    )

    # 预处理后尺寸
    h, w = image.shape
    if (h, w) != (h0, w0):
        print(f"预处理后图像尺寸：{w} x {h}")

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        stars_all, stats, mask, labels = detect_stars(
            image,
            detect_sigma=DETECT_SIGMA,
            snr_threshold=SNR_THRESHOLD,
            min_area=MIN_AREA,
            border_margin=BORDER_MARGIN,
            max_stars=MAX_STARS,
        )

    # 为提速，不再打印背景与星点数量等中间信息，只关注 HFR 结果

    if len(stars_all) == 0:
        median_hfr = 0.0
        print("没有找到满足条件的星点，median_HFR 记为 0.0")
        print(f"{filename}: median_HFR = {median_hfr:.4f} 像素（基于 0 个星点）")
        return median_hfr

    # 计算并输出所有星点 HFR 的中位数（保持原有 HFR 逻辑不变）
    hfr_all = np.array([s.get("hfr", 0.0) for s in stars_all], dtype=float)
    hfr_valid = hfr_all[hfr_all > 0]
    if hfr_valid.size == 0:
        median_hfr = 0.0
        print("所有星点的 HFR 均无效，median_HFR 记为 0.0")
    else:
        median_hfr = float(np.median(hfr_valid))
    print(f"{filename}: median_HFR = {median_hfr:.4f} 像素（基于 {hfr_valid.size} 个星点）")

    return median_hfr


def main():
    parser = argparse.ArgumentParser(
        description="根据星点 HFR 评估单张粗对焦 FITS 图像，对每张图输出 median_HFR。"
    )
    parser.add_argument("files", nargs="+", help="要处理的 FITS 文件列表")
    args = parser.parse_args()

    for f in args.files:
        process_file(f)


if __name__ == "__main__":
    main()
