
# -*- coding: utf-8 -*-
"""
加速版星点识别脚本（将 LoG 候选检测替换为 DoG 差分高斯以显著提速）
- 用 skimage.feature.blob_dog 近似替换 blob_log，sigma_ratio=1.6
- 阈值匹配：dog_threshold = log_threshold * 0.67
- 输出半径仍为 r ≈ √2 * σ（与原脚本一致）
- 入口与输出格式：打印"最终HFR = ... 像素"和"HFR:..."，失败时退出码=2
- 使用HFR（Half Flux Radius）替代FWHM进行星点质量评估
"""
import os
import cv2
import sys
import json
import time
import numpy as np
from concurrent.futures import ThreadPoolExecutor, as_completed
from skimage import io, feature, color
from skimage.feature import blob_dog
from scipy.optimize import curve_fit

def gaussian_1d(x, amplitude, center, sigma, offset):

    
    return offset + amplitude * np.exp(-(x - center)**2 / (2 * sigma**2))

def radial_profile(region, center):
    """优化的径向轮廓计算 - 使用预计算的距离矩阵和向量化操作"""
    h, w = region.shape
    if h == 0 or w == 0:
        return np.array([0.0], dtype=np.float32)
    
    # 预计算距离矩阵 - 使用更高效的实现
    y, x = np.ogrid[:h, :w]
    r_squared = (x - center[0])**2 + (y - center[1])**2
    r = np.sqrt(r_squared)
    r_int = r.astype(np.int32)
    
    if r_int.size == 0:
        return np.array([0.0], dtype=np.float32)
    
    maxr = int(r_int.max())
    if maxr == 0:
        return np.array([float(region[center[1], center[0]])], dtype=np.float32)
    
    # 使用更高效的bincount操作
    r_flat = r_int.ravel()
    vals_flat = region.ravel()
    
    # 预分配数组避免重复分配
    sums = np.bincount(r_flat, weights=vals_flat, minlength=maxr + 1)
    counts = np.bincount(r_flat, minlength=maxr + 1)
    
    # 向量化除法操作
    result = np.zeros(maxr + 1, dtype=np.float32)
    valid_mask = counts > 0
    result[valid_mask] = sums[valid_mask] / counts[valid_mask]
    
    return result

def calculate_hfr(region, center):
    """优化的HFR计算 - 减少内存分配和计算复杂度"""
    h, w = region.shape
    if h < 3 or w < 3:
        return 0.0
    
    # 优化的背景计算 - 只计算边界像素
    border_pixels = np.concatenate([
        region[0, :], region[-1, :], region[:, 0], region[:, -1]
    ])
    background = np.median(border_pixels)

    # 原地操作减少内存分配
    region_bg_removed = region - background
    region_bg_removed = np.maximum(region_bg_removed, 0)  # 更高效的clip操作

    radial_intensity = radial_profile(region_bg_removed, center)
    if len(radial_intensity) < 3:
        return 0.0

    # 计算总光量
    total_flux = np.sum(radial_intensity)
    if total_flux <= 0:
        return 0.0
    
    half_flux = total_flux * 0.5  # 避免除法
    
    # 计算累积光量分布 - 使用更高效的实现
    cumulative_flux = np.cumsum(radial_intensity)
    
    # 优化的HFR半径查找
    try:
        # 使用searchsorted进行二分查找，比argmax更快
        hfr_idx = np.searchsorted(cumulative_flux, half_flux, side='right')
        
        if hfr_idx == 0:
            if cumulative_flux[0] >= half_flux:
                hfr_radius = 0.5
            else:
                hfr_radius = 0.0
        elif hfr_idx >= len(cumulative_flux):
            hfr_radius = float(len(cumulative_flux) - 1)
        else:
            # 线性插值
            flux_before = cumulative_flux[hfr_idx - 1]
            flux_after = cumulative_flux[hfr_idx]
            if flux_after > flux_before:
                ratio = (half_flux - flux_before) / (flux_after - flux_before)
                hfr_radius = hfr_idx - 1 + ratio
            else:
                hfr_radius = float(hfr_idx)
            
        return max(0.0, hfr_radius)
    except Exception:
        return 3.0

def is_too_close(star, star_list, min_dist=10):
    x1, y1 = star["position"]
    for s in star_list:
        x2, y2 = s["position"]
        if np.hypot(x1 - x2, y1 - y2) < min_dist:
            return True
    return False

def _percentile_stretch(img, p_low=1.0, p_high=99.0):
    finite = np.isfinite(img)
    if not np.any(finite):
        return np.zeros_like(img, dtype=np.float32)
    vmin = np.percentile(img[finite], p_low)
    vmax = np.percentile(img[finite], p_high)
    if vmax <= vmin:
        vmin, vmax = np.min(img[finite]), np.max(img[finite])
        if vmax <= vmin:
            return np.zeros_like(img, dtype=np.float32)
    out = (img - vmin) / (vmax - vmin)
    return np.clip(out, 0, 1).astype(np.float32)

def _binning_mean(img, target_max_dim=1000):
    """优化的图像缩放 - 使用更高效的实现"""
    h, w = img.shape[:2]
    if max(h, w) <= target_max_dim:
        return img.astype(np.float32)

    bin_factor = int(np.ceil(max(h, w) / float(target_max_dim)))
    bin_factor = max(1, bin_factor)

    new_h = (h // bin_factor) * bin_factor
    new_w = (w // bin_factor) * bin_factor
    if new_h <= 0 or new_w <= 0:
        return img.astype(np.float32)

    y0 = (h - new_h) // 2
    x0 = (w - new_w) // 2
    crop = img[y0:y0+new_h, x0:x0+new_w]

    # 使用更高效的reshape和mean操作
    out = crop.reshape(new_h // bin_factor, bin_factor,
                       new_w // bin_factor, bin_factor).mean(axis=(1, 3))
    return out.astype(np.float32)

def _file_sig(path):
    try:
        st = os.stat(path)
        return f"{int(st.st_mtime)}_{st.st_size}"
    except FileNotFoundError:
        return "NA"

def _safe_write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def prepare_image_if_fits(input_path, target_max_dim=1000):
    ext = os.path.splitext(input_path)[1].lower()
    # For non-FITS inputs, no preprocessing and bin_factor=1
    if ext not in (".fits", ".fit", ".fts"):
        return input_path, 1

    out_png = os.path.splitext(input_path)[0] + "_prepped.png"
    meta_path = out_png + ".json"
    param_sig = {"target_max_dim": int(target_max_dim)}
    inp_sig = _file_sig(input_path)

    # Try load cache
    try:
        with open(meta_path, "r", encoding="utf-8") as f:
            meta = json.load(f)
        if (meta.get("input_sig") == inp_sig and
            meta.get("param_sig") == param_sig and
            os.path.exists(out_png)):
            bin_factor_cached = int(meta.get("bin_factor", 1))
            return out_png, bin_factor_cached
    except Exception:
        pass

    # Read FITS and pick a 2D plane
    from astropy.io import fits
    hdul = fits.open(input_path)
    data = None
    try:
        for hdu in hdul:
            if hasattr(hdu, 'data') and hdu.data is not None:
                arr = np.asanyarray(hdu.data)
                if arr.ndim == 2:
                    data = arr.astype(np.float32); break
                elif arr.ndim > 2:
                    slicer = [0] * (arr.ndim - 2)
                    data = arr[tuple(slicer)].astype(np.float32); break
    finally:
        hdul.close()
    if data is None:
        raise ValueError("FITS 文件中未找到有效的二维图像数据。")

    finite = np.isfinite(data)
    if not np.any(finite):
        raise ValueError("FITS 数据全部为非有限值（NaN/Inf）。")
    med = np.median(data[finite])
    data[~finite] = med

    h, w = data.shape[:2]
    bin_factor = int(np.ceil(max(h, w) / float(target_max_dim)))
    bin_factor = max(1, bin_factor)

    # Use existing utilities if present
    if '_percentile_stretch' in globals():
        data = _percentile_stretch(data, 1.0, 99.0)

    if '_binning_mean' in globals():
        data = _binning_mean(data, target_max_dim=target_max_dim)
    else:
        # Fallback: simple block reduce via reshape (approximate)
        # Ensure divisibility
        import math
        bf = bin_factor
        nh = h // bf * bf
        nw = w // bf * nw if 'nw' in locals() else w // bf * bf
        data = data[:nh, :nw]
        data = data.reshape(nh//bf, bf, nw//bf, bf).mean(axis=(1,3))

    if '_percentile_stretch' in globals():
        data = _percentile_stretch(data, 1.0, 99.0)

    png_u8 = (np.clip(data, 0, 1) * 255.0).round().astype(np.uint8)
    cv2.imwrite(out_png, png_u8)

    meta = {
        "input_sig": inp_sig,
        "param_sig": param_sig,
        "bin_factor": int(bin_factor),
        "created_at": int(time.time())
    }
    try:
        _safe_write_json(meta_path, meta)
    except Exception:
        # best-effort
        pass

    return out_png, bin_factor

def _largest_rectangle_inside_mask(mask):
    h, w = mask.shape
    heights = np.zeros((h, w), dtype=int)
    heights[0] = mask[0]
    for i in range(1, h):
        heights[i] = (heights[i - 1] + 1) * mask[i]

    best_area, best = 0, (0, 0, 0, 0)
    for i in range(h):
        hist = heights[i]
        stack = []
        j = 0
        while j <= w:
            curr = hist[j] if j < w else 0
            if not stack or curr >= hist[stack[-1]]:
                stack.append(j); j += 1
            else:
                top = stack.pop()
                height = hist[top]
                width = j if not stack else j - stack[-1] - 1
                area = height * width
                if area > best_area:
                    x2 = j - 1
                    x1 = 0 if not stack else stack[-1] + 1
                    y2 = i
                    y1 = i - height + 1
                    best_area, best = area, (x1, y1, x2, y2)
    return best

def auto_crop_image(input_img_path, erode_frac=0.09, min_keep_ratio=0.5, save_suffix="_crop.png"):
    img = cv2.imread(input_img_path, cv2.IMREAD_GRAYSCALE)
    if img is None:
        raise FileNotFoundError(f"无法读取图像: {input_img_path}")
    h, w = img.shape

    out_path = os.path.splitext(input_img_path)[0] + save_suffix
    meta_path = out_path + ".json"
    param_sig = {"erode_frac": float(erode_frac), "min_keep_ratio": float(min_keep_ratio)}
    inp_sig = _file_sig(input_img_path)

    try:
        with open(meta_path, "r", encoding="utf-8") as f:
            meta = json.load(f)
        if meta.get("input_sig") == inp_sig and meta.get("param_sig") == param_sig and os.path.exists(out_path):
            ratios = meta.get("ratios", {})
            return out_path, ratios
    except Exception:
        pass

    blur = cv2.GaussianBlur(img, (0, 0), 3)
    _, mask = cv2.threshold(blur, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    mask = (mask > 0).astype(np.uint8)

    k = max(3, int(erode_frac * min(h, w)))
    if k % 2 == 0:
        k += 1
    ker = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
    mask = cv2.erode(mask, ker, iterations=1)

    x1, y1, x2, y2 = _largest_rectangle_inside_mask(mask)

    keep_area = (y2 - y1 + 1) * (x2 - x1 + 1)
    if keep_area <= 0 or keep_area < min_keep_ratio * (h * w):
        m = max(2, int(0.01 * min(h, w)))
        x1, y1, x2, y2 = m, m, w - m - 1, h - m - 1

    cropped = img[y1:y2+1, x1:x2+1]
    cv2.imwrite(out_path, cropped)

    ratios = {
        "left": round(x1 / w, 4),
        "right": round(1 - (x2 + 1) / w, 4),
        "top": round(y1 / h, 4),
        "bottom": round(1 - (y2 + 1) / h, 4),
        "crop_box_pixels": [int(x1), int(y1), int(x2), int(y2)],
        "image_size": (w, h)
    }
    meta = {"input_sig": inp_sig, "param_sig": param_sig, "ratios": ratios, "created_at": int(time.time())}
    _safe_write_json(meta_path, meta)
    return out_path, ratios

def dynamic_radius_from_region(star_region, center_rc, fallback_radius):
    if star_region.size == 0:
        return fallback_radius

    img = star_region
    if img.dtype != np.uint8:
        imin, imax = img.min(), img.max()
        if imax > imin:
            img = ((img - imin) / (imax - imin) * 255.0).astype(np.uint8)
        else:
            return fallback_radius

    try:
        blur = cv2.GaussianBlur(img, (3, 3), 0.8)
        _, mask = cv2.threshold(blur, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    except Exception:
        t = max(5, int(0.3 * int(img.max())))
        _, mask = cv2.threshold(img, t, 255, cv2.THRESH_BINARY)

    m = (mask > 0).astype(np.uint8)
    num, labels, stats, _ = cv2.connectedComponentsWithStats(m, connectivity=8)
    if num <= 1:
        return fallback_radius

    max_idx = 1 + np.argmax(stats[1:, cv2.CC_STAT_AREA])
    comp = (labels == max_idx).astype(np.uint8)

    kernel = np.ones((3, 3), np.uint8)
    edge = cv2.subtract(comp, cv2.erode(comp, kernel, iterations=1))
    py, px = np.nonzero(edge)
    if len(px) < 5:
        return fallback_radius

    pts = np.column_stack((px, py)).astype(np.float32)
    (_, _), r = cv2.minEnclosingCircle(pts)

    h, w = star_region.shape[:2]
    r = float(r)
    r = max(2.0, min(r, 0.6 * max(h, w)))
    return r

def assess_circularity(star_region, center_rc, min_circularity=0.60, min_axis_ratio=0.75, max_center_offset_ratio=0.35):
    if star_region.size == 0:
        return False, 0.0, {"reason": "empty"}

    img = star_region
    if img.dtype != np.uint8:
        imin, imax = img.min(), img.max()
        if imax > imin:
            img = ((img - imin) / (imax - imin) * 255.0).astype(np.uint8)
        else:
            return False, 0.0, {"reason": "flat"}

    blur = cv2.GaussianBlur(img, (3, 3), 0.8)
    try:
        _, mask = cv2.threshold(blur, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    except Exception:
        t = max(5, int(0.3 * int(img.max())))
        _, mask = cv2.threshold(blur, t, 255, cv2.THRESH_BINARY)

    m = (mask > 0).astype(np.uint8)
    num, labels, stats, _ = cv2.connectedComponentsWithStats(m, connectivity=8)
    if num <= 1:
        return False, 0.0, {"reason": "no_component"}

    max_idx = 1 + np.argmax(stats[1:, cv2.CC_STAT_AREA])
    comp = (labels == max_idx).astype(np.uint8)

    contours, _ = cv2.findContours(comp, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return False, 0.0, {"reason": "no_contour"}
    cnt = max(contours, key=cv2.contourArea)

    A = cv2.contourArea(cnt)
    P = cv2.arcLength(cnt, True)
    if P <= 1 or A <= 1:
        return False, 0.0, {"reason": "degenerate"}

    circularity = float(4.0 * np.pi * A / (P * P))

    M = cv2.moments(cnt)
    if M["m00"] != 0:
        cx = M["m10"] / M["m00"]
        cy = M["m01"] / M["m00"]
    else:
        cx = np.mean(cnt[:, 0, 0])
        cy = np.mean(cnt[:, 0, 1])

    (ecx, ecy), enr = cv2.minEnclosingCircle(cnt)
    enr = max(enr, 1e-3)

    cy0, cx0 = center_rc
    center_offset = np.hypot(cx - cx0, cy - cy0)
    center_offset_ratio = float(center_offset / enr)

    if len(cnt) >= 5:
        ellipse = cv2.fitEllipse(cnt)
        (ex, ey), (MA, ma), angle = ellipse
        a = max(MA, ma) / 2.0
        b = min(MA, ma) / 2.0
        axis_ratio = float(b / a) if a > 1e-3 else 0.0
    else:
        pts = cnt.reshape(-1, 2).astype(np.float32)
        rs = np.hypot(pts[:, 0] - ecx, pts[:, 1] - ecy)
        if rs.size >= 3:
            axis_ratio = float(np.clip(np.percentile(rs, 10) / np.percentile(rs, 90), 0.0, 1.0))
        else:
            axis_ratio = 0.0

    ok_circ = (circularity >= min_circularity)
    ok_axis = (axis_ratio >= min_axis_ratio)
    ok_center = (center_offset_ratio <= max_center_offset_ratio)
    is_circular = bool(ok_circ and ok_axis and ok_center)

    score = (0.5 * circularity) + (0.4 * axis_ratio) + (0.1 * max(0.0, 1.0 - center_offset_ratio))
    debug = {
        "circularity": circularity,
        "axis_ratio": axis_ratio,
        "center_offset_ratio": center_offset_ratio,
        "ok_circ": ok_circ,
        "ok_axis": ok_axis,
        "ok_center": ok_center
    }
    return is_circular, float(score), debug

def choose_final_hfr(selected_stars):
    stars = [s for s in selected_stars if s.get("hfr", 0) > 0]
    if not stars:
        return None, {"reason": "no_valid_stars"}

    if len(stars) == 1:
        return float(stars[0]["hfr"]), {"rule": "one_star", "picked": stars[0]}

    if len(stars) == 2:
        bs = [float(s.get("brightness", 0.0)) for s in stars]
        bmin, bmax = float(min(bs)), float(max(bs))
        if bmax - bmin < 1e-9:
            norm = [0.5, 0.5]
        else:
            norm = [(b - bmin) / (bmax - bmin) for b in bs]

        scores = []
        for s, bnorm in zip(stars, norm):
            circ = float(s.get("circularity_score", 0.0))
            non_border = 1.0 if not s.get("on_border", False) else 0.0
            rel = 0.6 * circ + 0.3 * bnorm + 0.1 * non_border
            scores.append(rel)

        best_idx = int(np.argmax(scores))
        return float(stars[best_idx]["hfr"]), {"rule": "two_stars_pick_best", "picked": stars[best_idx]}

    # 对于3颗或更多星点，取中位数
    hfrs = sorted(float(s["hfr"]) for s in stars)
    median_hfr = float(np.median(hfrs))
    return median_hfr, {"rule": "multiple_stars_median", "all_hfrs": hfrs, "count": len(stars)}

def _process_one_blob(blob_idx, blob, image, H, W, require_circular=True):
    y, x, r = blob
    x_int = int(x); y_int = int(y)

    region_radius = min(max(int(r * 1.5), 5), 50)
    x_min = max(0, x_int - region_radius)
    x_max = min(W, x_int + region_radius + 1)
    y_min = max(0, y_int - region_radius)
    y_max = min(H, y_int + region_radius + 1)
    if x_max <= x_min or y_max <= y_min:
        return None

    star_region = image[y_min:y_max, x_min:x_max]
    brightness = float(np.max(star_region))

    cx_local = x_int - x_min
    cy_local = y_int - y_min

    is_circ, circ_score, _ = assess_circularity(
        star_region, (cy_local, cx_local),
        min_circularity=0.60,
        min_axis_ratio=0.75,
        max_center_offset_ratio=0.35
    )
    if require_circular and (not is_circ):
        return None

    hfr = calculate_hfr(star_region, (cx_local, cy_local))
    dynamic_r_local = dynamic_radius_from_region(
        star_region, (cy_local, cx_local), fallback_radius=region_radius
    )
    dynamic_r_global = int(round(dynamic_r_local))

    on_border = (x_min == 0 or x_max == W or y_min == 0 or y_max == H)

    return {
        "idx": int(blob_idx),
        "position": (x_int, y_int),
        "radius_region": region_radius,
        "radius_dynamic": dynamic_r_global,
        "hfr": float(hfr),
        "brightness": brightness,
        "region": (x_min, y_min, x_max, y_max),
        "on_border": bool(on_border),
        "is_circular": True,
        "circularity_score": float(circ_score)
    }

def detect_stars_hfr_only(image_path, require_circular=True, max_workers=None,
                           min_sigma=3, max_sigma=20, num_sigma=15, threshold=0.01):
    image = io.imread(image_path, as_gray=True)
    if len(image.shape) == 3 and image.shape[2] == 3:
        image = color.rgb2gray(image)

    DOG_K = 1.6
    dog_threshold = float(threshold) * 0.67
    blobs = blob_dog(
        image,
        min_sigma=min_sigma,
        max_sigma=max_sigma,
        sigma_ratio=DOG_K,
        threshold=dog_threshold
    )
    if blobs is None or len(blobs) == 0:
        return None, []

    blobs[:, 2] = blobs[:, 2] * np.sqrt(2)

    H, W = image.shape[:2]
    results = []
    workers = max_workers or min(32, (os.cpu_count() or 1))
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futures = []
        for i, b in enumerate(blobs):
            futures.append(ex.submit(_process_one_blob, i, b, image, H, W, require_circular))
        tmp = [None] * len(futures)
        for fut in as_completed(futures):
            res = fut.result()
            if res is not None:
                tmp[res["idx"]] = res
        results = [r for r in tmp if r is not None]

    if not results:
        return None, []

    results.sort(key=lambda s: (0 if not s["on_border"] else 1, -s["brightness"]))
    final_top_stars, backup = [], list(results)
    # 优先选择非边界且HFR合理的星点
    while len(final_top_stars) < 9 and backup:
        cand = backup.pop(0)
        if cand["hfr"] < 3.0 or cand["on_border"]:
            repl = False
            for i, alt in enumerate(backup):
                if alt["hfr"] >= 3.0 and not alt["on_border"]:
                    if not is_too_close(alt, final_top_stars):
                        final_top_stars.append(alt); backup.pop(i); repl = True; break
            if not repl and not is_too_close(cand, final_top_stars):
                final_top_stars.append(cand)
        else:
            if not is_too_close(cand, final_top_stars):
                final_top_stars.append(cand)
    # 如果还不够9颗星，继续添加其他星点
    while len(final_top_stars) < 9 and backup:
        cand = backup.pop(0)
        if not is_too_close(cand, final_top_stars):
            final_top_stars.append(cand)

    star_data_min = [{
        "position": s["position"],
        "radius_dynamic": float(s["radius_dynamic"]),
        "hfr": float(s["hfr"]),
        "on_border": bool(s["on_border"]),
        "is_circular": True,
        "circularity_score": float(s.get("circularity_score", 0.0)),
        "brightness": float(s.get("brightness", 0.0))
    } for s in final_top_stars]

    final_hfr, _ = choose_final_hfr(star_data_min)
    return final_hfr, star_data_min

# =========================
# 入口（仅输出最终 HFR）
# =========================
if __name__ == "__main__":
    cv2.setUseOptimized(True)
    image_path = sys.argv[1] if len(sys.argv) > 1 else "/dev/shm/ccd_simulator.fits"

    try:
        prepared_path, __bin_factor = prepare_image_if_fits(image_path, target_max_dim=1000)
    except Exception as e:
        print(f"最终HFR：未能计算（读取/预处理失败：{e}）。")
        sys.exit(2)

    try:
        cropped_path, _ = auto_crop_image(prepared_path, erode_frac=0.09, min_keep_ratio=0.5)
    except Exception:
        cropped_path = prepared_path

    final_hfr, _ = detect_stars_hfr_only(cropped_path, require_circular=True)

    if final_hfr is None or not np.isfinite(final_hfr) or final_hfr <= 0:
        print("最终HFR：未能计算（无有效星点）。")
        sys.exit(2)
    else:
        print(f"最终HFR = {final_hfr:.6f} 像素")
        print(f"HFR:{final_hfr:.6f}")
        sys.exit(0)
try:
    __val_candidates = [final_hfr]
    __val = None
    for _v in __val_candidates:
        if isinstance(_v, (int, float)) and _v > 0:
            __val = float(_v); break
    if __val is not None:
        # 输出处理后像素与原始像素的HFR
        print(f"最终HFR(处理后像素) = {__val:.6f} 像素")
        print(f"最终HFR(原始像素)   = {__val * float(__bin_factor):.6f} 像素  [bin_factor={__bin_factor}]")
        print(f"HFR:{__val * float(__bin_factor):.6f}")
except Exception as e:
    # Do not crash on printing
    pass
