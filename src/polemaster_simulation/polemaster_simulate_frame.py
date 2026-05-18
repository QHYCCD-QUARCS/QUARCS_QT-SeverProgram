#!/usr/bin/env python3
"""PoleMaster simulation frame generator (migrated layout).

This variant keeps QUARCS JSON/FITS outputs, but image rendering is delegated to
the C++ renderer from /simulate_astro_images/cpp/render_sky_patch.cpp.
"""

from __future__ import annotations

import argparse
import importlib
import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np

STARTUP_WARNINGS: list[str] = []


def auto_install_enabled() -> bool:
    flag = os.environ.get("QUARCS_AUTO_INSTALL_PY_DEPS", "1").strip().lower()
    return flag not in {"0", "false", "off", "no"}


def ensure_python_module(module_name: str, pip_name: str) -> object | None:
    try:
        return importlib.import_module(module_name)
    except Exception as first_error:
        if not auto_install_enabled():
            STARTUP_WARNINGS.append(f"dependency-missing:{pip_name}:{first_error}")
            return None

        pip_cmd = [sys.executable, "-m", "pip", "install", pip_name]
        proc = subprocess.run(pip_cmd, capture_output=True, text=True, timeout=180)
        if proc.returncode != 0:
            err = (proc.stderr or proc.stdout or "").strip().replace("\n", " ")
            STARTUP_WARNINGS.append(f"dependency-install-failed:{pip_name}:{err[:220]}")
            return None

        try:
            STARTUP_WARNINGS.append(f"dependency-installed:{pip_name}")
            return importlib.import_module(module_name)
        except Exception as second_error:
            STARTUP_WARNINGS.append(f"dependency-import-failed:{pip_name}:{second_error}")
            return None


fits_module = ensure_python_module("astropy.io.fits", "astropy")
fits = fits_module


TRUE_POLE_RA_DEG = 0.0
NORTH_POLE_DEC_DEG = 89.9999
SOUTH_POLE_DEC_DEG = -89.9999

POLE_PATH = [
    (668.0, 505.0), (648.0, 493.0), (626.0, 501.0), (606.0, 474.0),
    (587.0, 482.0), (574.0, 456.0), (552.0, 463.0), (545.0, 440.0),
    (532.0, 449.0), (526.0, 427.0), (516.0, 433.0), (522.0, 414.0),
    (511.0, 420.0), (516.0, 404.0), (507.0, 410.0), (514.0, 397.0),
    (506.0, 401.0), (512.0, 392.0), (507.0, 395.0), (512.8, 389.8),
    (508.2, 392.0), (512.4, 388.2), (509.2, 390.0), (512.2, 386.9),
    (510.0, 388.2), (512.0, 386.0), (510.8, 386.8), (512.0, 385.3),
    (511.2, 385.7), (512.5, 384.9), (511.8, 384.6), (512.3, 384.4),
    (512.1, 384.2), (512.0, 384.1),
]

NORTH_FIXED_STARS = [
    ("N1", "Polaris", 37.95456067, 89.26410897, 1.98),
    ("N2", "Lambda UMi", 259.238583, 89.037722, 6.31),
    ("N3", "UY UMi", 183.836125, 87.700000, 6.27),
    ("N4", "24 UMi", 262.695708, 86.968028, 5.78),
    ("N5", "HD 5914", 23.452167, 89.015722, 6.46),
]

SOUTH_FIXED_STARS = [
    ("S1", "Sigma Octantis", 317.191708, -88.956500, 5.45),
    ("S2", "Chi Octantis", 283.698542, -87.605528, 5.29),
    ("S3", "Tau Octantis", 352.014875, -87.482250, 5.50),
    ("S4", "R Octantis", 81.525500, -86.388278, 6.40),
    ("S5", "HD 107739", 186.409500, -86.150583, 6.32),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a PoleMaster simulation frame.")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--fits-dir", type=Path, default=Path("/dev/shm"))
    parser.add_argument("--fits-name", type=str, default="polecamera.fits")
    parser.add_argument("--catalog", type=Path, default=None)
    parser.add_argument("--frame-index", type=int, default=0)
    parser.add_argument("--script-index", type=int, default=0)
    parser.add_argument("--state-number", type=int, default=1)
    parser.add_argument("--exposure-ms", type=int, default=1000)
    parser.add_argument("--hemisphere", choices=["north", "south"], default="north")
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--axis-x", type=float, default=512.0)
    parser.add_argument("--axis-y", type=float, default=384.0)
    parser.add_argument("--fov", type=float, default=12.0)
    parser.add_argument("--fov-y", type=float, default=8.0)
    parser.add_argument("--max-mag", type=float, default=10.0)
    parser.add_argument("--pole-x", type=float, default=float("nan"))
    parser.add_argument("--pole-y", type=float, default=float("nan"))
    parser.add_argument("--roll-deg", type=float, default=float("nan"))
    parser.add_argument("--psf-sigma", type=float, default=1.15)
    parser.add_argument("--gain", type=float, default=2.0)
    parser.add_argument("--lim-mag-t0", type=float, default=1.0, help="Reference exposure time in seconds")
    parser.add_argument("--lim-mag-m0", type=float, default=6.0, help="Limiting magnitude at reference exposure")
    parser.add_argument("--flux-mag-ref", type=float, default=6.0, help="Reference magnitude for brightness scaling")
    parser.add_argument("--flux-scale", type=float, default=22.0, help="Global brightness scale factor")
    parser.add_argument("--flux-visibility-threshold", type=float, default=0.035, help="Stars with flux below this threshold are hidden")
    return parser.parse_args()


def default_simulate_root() -> Path:
    env_root = os.environ.get("SIMULATE_ASTRO_IMAGES_ROOT", "").strip()
    if env_root:
        p = Path(env_root).resolve()
        if p.exists():
            return p
    here = Path(__file__).resolve()
    candidates = [
        here.parents[3] / "simulate_astro_images",
        Path("/home/quarcs/workspace/QUARCS/simulate_astro_images"),
        Path.cwd() / "simulate_astro_images",
    ]
    for c in candidates:
        if c.exists():
            return c.resolve()
    raise FileNotFoundError("simulate_astro_images root not found; set SIMULATE_ASTRO_IMAGES_ROOT")


def default_catalog_path(sim_root: Path) -> Path:
    return sim_root / "data" / "hip_catalog.csv"


def local_simulation_dir() -> Path:
    return Path(__file__).resolve().parent


def resolve_catalog_path(explicit_catalog: Path | None) -> Path | None:
    if explicit_catalog is not None:
        p = explicit_catalog.resolve()
        return p if p.exists() else None

    local_dir = local_simulation_dir()
    candidates = [
        local_dir / "hip_catalog.csv",
        local_dir / "data" / "hip_catalog.csv",
    ]

    env_root = os.environ.get("SIMULATE_ASTRO_IMAGES_ROOT", "").strip()
    if env_root:
        candidates.append(Path(env_root).resolve() / "data" / "hip_catalog.csv")

    candidates.extend(
        [
            Path("/home/quarcs/workspace/QUARCS/simulate_astro_images/data/hip_catalog.csv"),
            Path.cwd() / "simulate_astro_images" / "data" / "hip_catalog.csv",
        ]
    )
    for path in candidates:
        if path.exists():
            return path.resolve()
    return None


def resolve_renderer_layout() -> tuple[Path | None, Path | None]:
    local_dir = local_simulation_dir()
    local_src = local_dir / "render_sky_patch.cpp"
    local_exe = local_dir / "render_sky_patch"
    if local_src.exists():
        return local_src, local_exe

    env_root = os.environ.get("SIMULATE_ASTRO_IMAGES_ROOT", "").strip()
    if env_root:
        root = Path(env_root).resolve()
        src = root / "cpp" / "render_sky_patch.cpp"
        exe = root / "cpp" / "render_sky_patch"
        if src.exists():
            return src, exe

    fallback_root = Path("/home/quarcs/workspace/QUARCS/simulate_astro_images")
    src = fallback_root / "cpp" / "render_sky_patch.cpp"
    exe = fallback_root / "cpp" / "render_sky_patch"
    if src.exists():
        return src, exe
    return None, None


def point_json(point: tuple[float, float]) -> dict:
    x, y = point
    return {"x": float(x), "y": float(y), "valid": math.isfinite(x) and math.isfinite(y)}


def load_catalog(catalog_path: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    data = np.genfromtxt(catalog_path, delimiter=",", names=True, dtype=None, encoding="utf-8")
    return (
        np.asarray(data["ra"], dtype=np.float64),
        np.asarray(data["dec"], dtype=np.float64),
        np.asarray(data["mag"], dtype=np.float64),
    )


def angular_distance_deg(ra_deg: np.ndarray, dec_deg: np.ndarray, ra0_deg: float, dec0_deg: float) -> np.ndarray:
    ra = np.deg2rad(ra_deg)
    dec = np.deg2rad(dec_deg)
    ra0 = math.radians(ra0_deg)
    dec0 = math.radians(dec0_deg)
    cos_d = np.sin(dec) * math.sin(dec0) + np.cos(dec) * math.cos(dec0) * np.cos(ra - ra0)
    return np.rad2deg(np.arccos(np.clip(cos_d, -1.0, 1.0)))


def gnomonic_project(ra_deg: np.ndarray, dec_deg: np.ndarray, ra0_deg: float, dec0_deg: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    ra = np.deg2rad(ra_deg)
    dec = np.deg2rad(dec_deg)
    ra0 = math.radians(ra0_deg)
    dec0 = math.radians(dec0_deg)
    dra = ra - ra0
    cosc = np.sin(dec0) * np.sin(dec) + np.cos(dec0) * np.cos(dec) * np.cos(dra)
    visible = cosc > 0.0
    x = np.cos(dec) * np.sin(dra) / cosc
    y = (np.cos(dec0) * np.sin(dec) - np.sin(dec0) * np.cos(dec) * np.cos(dra)) / cosc
    return x, y, visible


def rotate_offsets(dx: np.ndarray, dy: np.ndarray, angle_deg: float) -> tuple[np.ndarray, np.ndarray]:
    theta = math.radians(angle_deg)
    c = math.cos(theta)
    s = math.sin(theta)
    return dx * c - dy * s, dx * s + dy * c


def project_to_pixels(
    ra: np.ndarray,
    dec: np.ndarray,
    pole_px: tuple[float, float],
    axis_px: tuple[float, float],
    fov_deg: float,
    fov_y_deg: float,
    width: int,
    height: int,
    roll_deg: float,
    pole_dec: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    x, y, visible = gnomonic_project(ra, dec, TRUE_POLE_RA_DEG, pole_dec)
    half_extent_x = math.tan(math.radians(fov_deg / 2.0))
    half_extent_y = math.tan(math.radians(fov_y_deg / 2.0))
    scale_x = width / (2.0 * half_extent_x)
    scale_y = height / (2.0 * half_extent_y)
    dx = x * scale_x
    dy = -y * scale_y
    dx, dy = rotate_offsets(dx, dy, roll_deg)
    axis_x, axis_y = axis_px
    pole_x, pole_y = pole_px
    base_x = axis_x + (pole_x - axis_x)
    base_y = axis_y + (pole_y - axis_y)
    return base_x + dx, base_y + dy, visible


def mag_to_flux_by_exposure(
    mag: np.ndarray,
    exposure_ms: int,
    mag_ref: float,
    flux_scale: float,
) -> np.ndarray:
    # Physical-style flux model:
    #   I ~ t * 10^(-0.4 * m)
    # Use a reference magnitude to keep numeric scale stable.
    t = max(0.01, float(exposure_ms) / 1000.0)
    rel = np.power(10.0, -0.4 * (mag - mag_ref))
    flux = flux_scale * t * rel
    # Prevent single bright stars from numerically dominating the frame.
    return np.clip(flux, 0.002, 2400.0)


def limiting_mag_by_exposure(exposure_ms: int, t0_s: float, m0: float) -> float:
    # m_lim(t) = m0 + 1.25 * log10(t / t0)
    t = max(0.01, float(exposure_ms) / 1000.0)
    t0 = max(0.01, float(t0_s))
    return float(m0 + 1.25 * math.log10(t / t0))


def gaussian_kernel1d(sigma: float) -> np.ndarray:
    radius = max(1, int(math.ceil(3.0 * max(0.1, sigma))))
    x = np.arange(-radius, radius + 1, dtype=np.float64)
    kernel = np.exp(-0.5 * (x / max(0.1, sigma)) ** 2)
    return kernel / np.sum(kernel)


def convolve_axis(image: np.ndarray, kernel: np.ndarray, axis: int) -> np.ndarray:
    return np.apply_along_axis(lambda row: np.convolve(row, kernel, mode="same"), axis, image)


def render_luma_image(
    width: int,
    height: int,
    x: np.ndarray,
    y: np.ndarray,
    mag: np.ndarray,
    sigma: float,
    gain: float,
    exposure_ms: int,
    mag_ref: float,
    flux_scale: float,
    flux_visibility_threshold: float,
    seed: int,
) -> np.ndarray:
    rng = np.random.default_rng(seed)
    field = rng.normal(0.008, 0.003, (height, width)).astype(np.float32)
    xi = np.rint(x).astype(np.int32)
    yi = np.rint(y).astype(np.int32)
    valid = (xi >= 0) & (xi < width) & (yi >= 0) & (yi < height)
    base_flux = mag_to_flux_by_exposure(mag, exposure_ms, mag_ref, flux_scale)
    # Hard visibility threshold: stars below threshold are not rendered at all.
    visible_flux_mask = valid & (base_flux >= float(max(0.0, flux_visibility_threshold)))
    if np.any(visible_flux_mask):
        np.add.at(
            field,
            (yi[visible_flux_mask], xi[visible_flux_mask]),
            base_flux[visible_flux_mask].astype(np.float32),
        )

    # Star profile varies with brightness:
    # bright stars -> slightly larger PSF; dim stars -> smaller PSF.
    dim_field = np.zeros_like(field)
    mid_field = np.zeros_like(field)
    bright_field = np.zeros_like(field)
    if np.any(visible_flux_mask):
        vis_flux = base_flux[visible_flux_mask]
        vxi = xi[visible_flux_mask]
        vyi = yi[visible_flux_mask]
        dim_band = vis_flux < 0.35
        mid_band = (vis_flux >= 0.35) & (vis_flux < 1.2)
        bright_band = vis_flux >= 1.2
        if np.any(dim_band):
            np.add.at(dim_field, (vyi[dim_band], vxi[dim_band]), vis_flux[dim_band].astype(np.float32))
        if np.any(mid_band):
            np.add.at(mid_field, (vyi[mid_band], vxi[mid_band]), vis_flux[mid_band].astype(np.float32))
        if np.any(bright_band):
            np.add.at(bright_field, (vyi[bright_band], vxi[bright_band]), vis_flux[bright_band].astype(np.float32))

    dim_kernel = gaussian_kernel1d(max(0.50, sigma * 0.70))
    mid_kernel = gaussian_kernel1d(max(0.65, sigma * 0.95))
    bright_kernel = gaussian_kernel1d(max(0.90, sigma * 1.30))
    blurred_dim = convolve_axis(convolve_axis(dim_field, dim_kernel, axis=1), dim_kernel, axis=0)
    blurred_mid = convolve_axis(convolve_axis(mid_field, mid_kernel, axis=1), mid_kernel, axis=0)
    blurred_bright = convolve_axis(convolve_axis(bright_field, bright_kernel, axis=1), bright_kernel, axis=0)
    blurred = blurred_dim + blurred_mid + blurred_bright
    signal = np.clip(blurred + 0.08 * np.sqrt(np.clip(field, 0.0, None)), 0.0, None)
    exposure_s = max(0.01, float(exposure_ms) / 1000.0)
    gain_scaled = gain * np.clip(0.82 + 0.08 * math.log10(1.0 + exposure_s * 8.0), 0.55, 1.45)
    luma = np.clip(1.0 - np.exp(-gain_scaled * signal), 0.0, 1.0)
    return np.rint(luma * 65535.0).astype(np.uint16)


def write_fits(
    path: Path,
    image_luma_16u: np.ndarray,
    pole_px: tuple[float, float],
    fov_deg: float,
    roll_deg: float,
    pole_dec: float,
) -> None:
    if fits is None:
        raise RuntimeError("astropy is unavailable")
    if image_luma_16u.ndim != 2:
        raise RuntimeError("invalid FITS image buffer shape")

    data = image_luma_16u.astype(np.uint16, copy=False)
    height, width = int(data.shape[0]), int(data.shape[1])
    pixel_scale = fov_deg / max(1, width)
    theta = math.radians(roll_deg)
    c = math.cos(theta)
    s = math.sin(theta)

    hdu = fits.PrimaryHDU(data)
    header = hdu.header
    header["CTYPE1"] = "RA---TAN"
    header["CTYPE2"] = "DEC--TAN"
    header["CUNIT1"] = "deg"
    header["CUNIT2"] = "deg"
    header["CRVAL1"] = TRUE_POLE_RA_DEG
    header["CRVAL2"] = pole_dec
    header["CRPIX1"] = float(pole_px[0]) + 1.0
    header["CRPIX2"] = float(pole_px[1]) + 1.0
    header["CD1_1"] = pixel_scale * c
    header["CD1_2"] = -pixel_scale * s
    header["CD2_1"] = -pixel_scale * s
    header["CD2_2"] = -pixel_scale * c
    header["EQUINOX"] = 2000.0
    header["RADESYS"] = "ICRS"
    header["SIMFOV"] = fov_deg
    header["SIMROLL"] = roll_deg
    hdu.writeto(path, overwrite=True)


def visible_sample_count(state_number: int) -> int:
    if state_number in (4, 5):
        return 2
    if state_number >= 6:
        return 3
    return 1


def roll_for_script_index(script_index: int, state_number: int) -> float:
    if state_number <= 3:
        return 0.0
    if state_number <= 5:
        return 35.0
    if state_number <= 7:
        return 70.0
    # Keep continuity when entering guiding stage; decay from calibration roll.
    if script_index <= 16:
        return 70.0
    if script_index <= 24:
        t = (script_index - 16) / 8.0
        return 70.0 * max(0.0, 1.0 - t)
    return 0.0


def pole_path_index_for_state(script_index: int, state_number: int) -> int:
    if state_number <= 3:
        return 0
    if state_number <= 5:
        return 8
    if state_number <= 7:
        return 16
    # Guiding stage should continue from calibration endpoint to avoid jumps.
    if script_index < 16:
        return 16
    return script_index


def synthetic_catalog(size: int = 600) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    rng = np.random.default_rng(0xA57E0123)
    ra = rng.uniform(0.0, 360.0, size=size)
    dec = rng.normal(loc=88.8, scale=0.8, size=size)
    dec = np.clip(dec, -89.9, 89.9)
    mag = rng.uniform(5.0, 9.8, size=size)
    return ra.astype(np.float64), dec.astype(np.float64), mag.astype(np.float64)

def main() -> None:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    args.fits_dir.mkdir(parents=True, exist_ok=True)
    catalog_path = resolve_catalog_path(args.catalog)

    pole_dec = NORTH_POLE_DEC_DEG if args.hemisphere == "north" else SOUTH_POLE_DEC_DEG
    fixed_catalog = NORTH_FIXED_STARS if args.hemisphere == "north" else SOUTH_FIXED_STARS
    script_index = max(0, min(args.script_index, len(POLE_PATH) - 1))
    pole_path_index = pole_path_index_for_state(script_index, args.state_number)
    pole_px_default = POLE_PATH[max(0, min(pole_path_index, len(POLE_PATH) - 1))]
    if math.isfinite(args.pole_x) and math.isfinite(args.pole_y):
        pole_px = (float(args.pole_x), float(args.pole_y))
    else:
        pole_px = pole_px_default
    axis_px = (args.axis_x, args.axis_y)
    roll_deg = float(args.roll_deg) if math.isfinite(args.roll_deg) else roll_for_script_index(script_index, args.state_number)

    file_name = f"PoleMasterSim_{args.frame_index:04d}.jpg"
    fits_name = args.fits_name.strip() if args.fits_name.strip() else "polecamera.fits"
    fits_path = args.fits_dir / fits_name

    warnings = list(STARTUP_WARNINGS)
    if catalog_path is not None:
        ra, dec, mag = load_catalog(catalog_path)
    else:
        warnings.append("catalog-missing:using-synthetic-stars")
        ra, dec, mag = synthetic_catalog()

    limiting_mag = limiting_mag_by_exposure(args.exposure_ms, args.lim_mag_t0, args.lim_mag_m0)
    # Keep the simulation catalog cap; limiting_mag controls detectability.
    effective_catalog_max_mag = float(max(0.0, args.max_mag))
    mask_mag = mag <= effective_catalog_max_mag
    ra, dec, mag = ra[mask_mag], dec[mask_mag], mag[mask_mag]
    dist = angular_distance_deg(ra, dec, TRUE_POLE_RA_DEG, pole_dec)
    mask_radius = dist <= args.fov * math.sqrt(2.0)
    ra, dec, mag = ra[mask_radius], dec[mask_radius], mag[mask_radius]
    x, y, visible = project_to_pixels(ra, dec, pole_px, axis_px, args.fov, args.fov_y, args.width, args.height, roll_deg, pole_dec)
    in_frame = visible & (x >= 0) & (x < args.width) & (y >= 0) & (y < args.height)
    x, y, mag = x[in_frame], y[in_frame], mag[in_frame]

    visible_mask = mag <= limiting_mag
    ignored_dim_star_count = int(np.count_nonzero(~visible_mask))
    x, y, mag = x[visible_mask], y[visible_mask], mag[visible_mask]

    fits_image = render_luma_image(
        width=args.width,
        height=args.height,
        x=x,
        y=y,
        mag=mag,
        sigma=args.psf_sigma,
        gain=args.gain,
        exposure_ms=args.exposure_ms,
        mag_ref=args.flux_mag_ref,
        flux_scale=args.flux_scale,
        flux_visibility_threshold=args.flux_visibility_threshold,
        seed=0x5EED1000 + args.frame_index * 97 + script_index * 131,
    )

    fits_generated = False
    try:
        write_fits(fits_path, fits_image, pole_px, args.fov, roll_deg, pole_dec)
        fits_generated = True
    except Exception as ex:
        warnings.append(f"fits-skipped:{str(ex)}")

    detected = [{"x": float(px), "y": float(py), "valid": True} for px, py in list(zip(x, y))[:120]]

    fixed_stars = []
    fixed_ra = np.array([s[2] for s in fixed_catalog], dtype=np.float64)
    fixed_dec = np.array([s[3] for s in fixed_catalog], dtype=np.float64)
    fx, fy, fvis = project_to_pixels(fixed_ra, fixed_dec, pole_px, axis_px, args.fov, args.fov_y, args.width, args.height, roll_deg, pole_dec)
    for i, star in enumerate(fixed_catalog):
        px = float(fx[i])
        py = float(fy[i])
        in_view = bool(fvis[i] and 0 <= px < args.width and 0 <= py < args.height)
        fixed_stars.append(
            {
                "id": star[0],
                "name": star[1],
                "raDeg": star[2],
                "decDeg": star[3],
                "mag": star[4],
                "expected": point_json((px, py)),
                "detected": point_json((px, py)) if in_view else point_json((-1.0, -1.0)),
                "matched": in_view,
                "visible": in_view,
                "distancePx": 0.0 if in_view else -1.0,
                "matchRadiusPx": 45.0,
            }
        )

    samples = []
    sample_path_indices = [0, 8, 16]
    sample_count = visible_sample_count(args.state_number)
    sample_ra = ra[:80]
    sample_dec = dec[:80]
    for idx in range(sample_count):
        sample_pole = POLE_PATH[sample_path_indices[idx]]
        sx, sy, svis = project_to_pixels(sample_ra, sample_dec, sample_pole, axis_px, args.fov, args.fov_y, args.width, args.height, idx * 35.0, pole_dec)
        sample_stars = []
        for px, py, ok in zip(sx, sy, svis):
            if ok and 0 <= px < args.width and 0 <= py < args.height:
                sample_stars.append(point_json((float(px), float(py))))
            if len(sample_stars) >= 24:
                break
        samples.append({"index": idx + 1, "pole": point_json(sample_pole), "stars": sample_stars})

    error_px = math.hypot(axis_px[0] - pole_px[0], axis_px[1] - pole_px[1])
    pixel_scale = ((args.fov * 3600.0 / args.width) + (args.fov_y * 3600.0 / args.height)) * 0.5
    phase = "guiding" if args.state_number == 8 else "simulation"
    overlay = {
        "phase": phase,
        "imageW": args.width,
        "imageH": args.height,
        "frameId": Path(file_name).stem,
        "hemisphere": args.hemisphere,
        "fixedStars": fixed_stars,
        "rotationSamples": samples,
        "axisCandidate": {
            **point_json(axis_px),
            "radiusPx": 122.0,
            "residualPx": 3.4,
        },
        "quality": {
            "starCount": int(len(x)),
            "ignoredDimStarCount": int(max(0, ignored_dim_star_count)),
            "fixedStarCount": len(fixed_stars),
            "fixedStarMatchedCount": sum(1 for s in fixed_stars if s["matched"]),
            "axisRadiusPx": 122.0,
            "axisResidualPx": 3.4,
            "lastRaRotationDeg": 35.0 if args.state_number >= 4 else 0.0,
            "method": "catalog-simulation-cpp-renderer",
            "exposureMs": int(max(1, args.exposure_ms)),
            "limitingMagnitude": float(limiting_mag),
            "catalogMaxMagnitude": float(effective_catalog_max_mag),
            "pixelScaleArcsecPerPixel": pixel_scale,
            "catalog": str(catalog_path) if catalog_path is not None else "",
            "renderer": "python-direct-fits",
        },
        "warnings": warnings,
    }
    result = {
        "fileName": file_name,
        "fitsFileName": fits_name,
        "fitsPath": str(fits_path.resolve()) if fits_generated else "",
        "imageW": args.width,
        "imageH": args.height,
        "guide": {
            "axisX": axis_px[0],
            "axisY": axis_px[1],
            "poleX": pole_px[0],
            "poleY": pole_px[1],
            "errorPx": error_px,
            "errorArcsec": error_px * pixel_scale,
            "pixelScaleArcsecPerPixel": pixel_scale,
        },
        "overlay": overlay,
    }
    print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))


if __name__ == "__main__":
    main()
