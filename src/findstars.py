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
from scipy.ndimage import label, find_objects, median_filter

import warnings

# 固定参数配置（根据原来命令行示例写死）
DETECT_SIGMA = 3.0       # 原脚本默认值
SNR_THRESHOLD = 3.0      # --snr-threshold 1
MIN_AREA = 4             # --min-area 4
BORDER_MARGIN = 10       # 原脚本默认值
MAX_STARS = 100          # --max-stars 100
MEDIAN_SIZE = 0          # 不做中值滤波
DOWNSAMPLE = 6           # --downsample 6

# 只有在 debug 模式才需要 matplotlib，避免无头环境报错
try:
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    HAS_MPL = True
except Exception:
    HAS_MPL = False


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

        # 说明：
        # - peak_snr 反映“单像素信噪比/锐利程度”，适合作为对焦好坏的评价指标；
        # - snr_total 是经典的总光子计数 SNR，星点越大、总通量越大就越高，
        #   这会导致“大而虚的星点 SNR 反而更高”，不利于对焦排序，因此只做参考。
        star = {
            "label": i + 1,
            "area": area,
            "peak_value": peak_value,
            "peak_snr": float(peak_snr),      # 用于对焦评价与排序
            "snr_total": float(snr_total),    # 物理意义上的总 SNR，仅供参考
            "y0": int(y0),
            "y1": int(y1),
            "x0": int(x0),
            "x1": int(x1),
            "cy": float(cy),
            "cx": float(cx),
        }
        stars.append(star)

    # 对焦评价：按“峰值 SNR”排序（星点越小越锐利，峰值 SNR 越高）
    stars_sorted = sorted(stars, key=lambda s: s["peak_snr"], reverse=True)
    if max_stars is None or max_stars <= 0:
        stars_top = stars_sorted
    else:
        stars_top = stars_sorted[:max_stars]

    return stars_sorted, stars_top, (median, sigma), mask, labels


def save_debug_images(image, mask, labels, stars, stars_top,
                      stats, out_dir, base_name):
    """保存 Debug 图像。"""
    if not HAS_MPL:
        print("警告：未能导入 matplotlib，无法输出 Debug 图像。")
        return

    os.makedirs(out_dir, exist_ok=True)
    median, sigma = stats

    # 为了显示效果做一个简单的线性拉伸
    vmin = median - 2 * sigma
    vmax = median + 10 * sigma

    # 1. 原始图
    fig, ax = plt.subplots(figsize=(8, 6))
    im = ax.imshow(image, origin='lower', vmin=vmin, vmax=vmax)
    ax.set_title(f"{base_name} - 原始图")
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    fig.savefig(os.path.join(out_dir, f"{base_name}_raw.png"), dpi=150)
    plt.close(fig)

    # 2. 阈值掩膜图
    fig, ax = plt.subplots(figsize=(8, 6))
    ax.imshow(mask, origin='lower', cmap='gray')
    ax.set_title(f"{base_name} - 阈值掩膜")
    fig.savefig(os.path.join(out_dir, f"{base_name}_mask.png"), dpi=150)
    plt.close(fig)

    # 3. 框出所有有效星点
    fig, ax = plt.subplots(figsize=(8, 6))
    ax.imshow(image, origin='lower', vmin=vmin, vmax=vmax)
    ax.set_title(f"{base_name} - 所有有效星点")
    for s in stars:
        rect = Rectangle((s["x0"], s["y0"]),
                         s["x1"] - s["x0"],
                         s["y1"] - s["y0"],
                         linewidth=0.5,
                         edgecolor='yellow',
                         facecolor='none')
        ax.add_patch(rect)
    fig.savefig(os.path.join(out_dir, f"{base_name}_all_stars.png"), dpi=150)
    plt.close(fig)

    # 4. 单独标出前 N 个星点（红色）
    fig, ax = plt.subplots(figsize=(8, 6))
    ax.imshow(image, origin='lower', vmin=vmin, vmax=vmax)
    ax.set_title(f"{base_name} - 前{len(stars_top)}个星点（按SNR）")
    # 先画所有，后画 top，使 top 更明显
    for s in stars:
        rect = Rectangle((s["x0"], s["y0"]),
                         s["x1"] - s["x0"],
                         s["y1"] - s["y0"],
                         linewidth=0.3,
                         edgecolor='yellow',
                         facecolor='none')
        ax.add_patch(rect)
    for s in stars_top:
        rect = Rectangle((s["x0"], s["y0"]),
                         s["x1"] - s["x0"],
                         s["y1"] - s["y0"],
                         linewidth=0.8,
                         edgecolor='red',
                         facecolor='none')
        ax.add_patch(rect)
    fig.savefig(os.path.join(out_dir, f"{base_name}_top_stars.png"), dpi=150)
    plt.close(fig)


def process_file(filename):
    """处理单个 FITS 文件，计算并输出基于所有星点 peak_snr 的平均值。

    说明：
        - 为兼容旧版 C++ 解析逻辑，仍然沿用字段名 avg_top50_snr；
        - 实际数值已经改为“所有检测到星点的平均 peak_snr”，不再对 50 做强行归一化；
        - 额外输出一行标准化结果：result=<value>，供 C++ 端稳定解析。
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
        stars_all, stars_top, stats, _, _ = detect_stars(
            image,
            detect_sigma=DETECT_SIGMA,
            snr_threshold=SNR_THRESHOLD,
            min_area=MIN_AREA,
            border_margin=BORDER_MARGIN,
            max_stars=MAX_STARS,
        )

    # 不再打印背景与星点数量，以提升速度

    if len(stars_all) == 0:
        avg_top50_snr = 0.0
        print("没有找到满足条件的星点，avg_top50_snr 记为 0.0")
        # 标准化输出，供 C++ 解析
        print(f"result={avg_top50_snr:.6f}")
        print(f"{filename}: avg_top50_snr = {avg_top50_snr:.6f}")
        return avg_top50_snr

    # 使用所有检测到的星点的 peak_snr 计算平均值
    snr_all = np.array([s["peak_snr"] for s in stars_all], dtype=float)
    if snr_all.size == 0:
        avg_top50_snr = 0.0
    else:
        avg_top50_snr = float(snr_all.mean())

    # 先输出标准化结果，再输出兼容旧版的人类可读结果
    print(f"result={avg_top50_snr:.6f}")
    print(f"{filename}: avg_top50_snr = {avg_top50_snr:.6f}")
    return avg_top50_snr


def main():
    parser = argparse.ArgumentParser(
        description="根据星点 SNR 评估粗对焦 FITS 图像，并给出排序结果。"
    )
    parser.add_argument("files", nargs="+", help="要处理的 FITS 文件列表")
    args = parser.parse_args()

    for f in args.files:
        process_file(f)


if __name__ == "__main__":
    main()
