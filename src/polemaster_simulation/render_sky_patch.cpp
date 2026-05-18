#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Options {
    double ra = 83.82;
    double dec = -5.3875;
    double fov = 30.0;
    fs::path out_path;
    fs::path catalog_path = "data/hip_catalog.csv";
    int dpi = 200;
    double size = 6.0;
    double max_mag = 10.0;
    double roll = 0.0;
    double psf_sigma = 1.2;
    double gain = 2.0;
};

struct Catalog {
    std::vector<double> ra;
    std::vector<double> dec;
    std::vector<double> mag;
};

struct Projected {
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> mag;
};

constexpr double kPi = 3.14159265358979323846;

double deg2rad(double deg) { return deg * kPi / 180.0; }
double rad2deg(double rad) { return rad * 180.0 / kPi; }

void print_usage() {
    std::cout << "Usage: render_sky_patch [options] --out <file>\n"
              << "Options:\n"
              << "  --ra <deg>\n"
              << "  --dec <deg>\n"
              << "  --fov <deg>\n"
              << "  --out <path>\n"
              << "  --catalog <path>\n"
              << "  --dpi <int>\n"
              << "  --size <float>\n"
              << "  --max-mag <float>\n"
              << "  --roll <deg>\n"
              << "  --psf-sigma <px>\n"
              << "  --gain <float>\n";
}

Options parse_args(int argc, char *argv[]) {
    Options opt;
    auto need_value = [&](int i, const std::string &key) {
        if (i + 1 >= argc)
            throw std::runtime_error("Missing value for " + key);
    };

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "-h" || key == "--help") {
            print_usage();
            std::exit(0);
        } else if (key == "--ra") {
            need_value(i, key);
            opt.ra = std::stod(argv[++i]);
        } else if (key == "--dec") {
            need_value(i, key);
            opt.dec = std::stod(argv[++i]);
        } else if (key == "--fov") {
            need_value(i, key);
            opt.fov = std::stod(argv[++i]);
        } else if (key == "--out") {
            need_value(i, key);
            opt.out_path = argv[++i];
        } else if (key == "--catalog") {
            need_value(i, key);
            opt.catalog_path = argv[++i];
        } else if (key == "--dpi") {
            need_value(i, key);
            opt.dpi = std::stoi(argv[++i]);
        } else if (key == "--size") {
            need_value(i, key);
            opt.size = std::stod(argv[++i]);
        } else if (key == "--max-mag") {
            need_value(i, key);
            opt.max_mag = std::stod(argv[++i]);
        } else if (key == "--roll") {
            need_value(i, key);
            opt.roll = std::stod(argv[++i]);
        } else if (key == "--psf-sigma") {
            need_value(i, key);
            opt.psf_sigma = std::stod(argv[++i]);
        } else if (key == "--gain") {
            need_value(i, key);
            opt.gain = std::stod(argv[++i]);
        } else {
            throw std::runtime_error("Unknown option: " + key);
        }
    }

    if (opt.out_path.empty())
        throw std::runtime_error("--out is required");
    if (!(opt.ra >= 0.0 && opt.ra < 360.0))
        throw std::runtime_error("--ra must be in [0,360)");
    if (!(opt.dec >= -90.0 && opt.dec <= 90.0))
        throw std::runtime_error("--dec must be in [-90,90]");
    if (!(opt.fov > 0.0 && opt.fov <= 170.0))
        throw std::runtime_error("--fov must be in (0,170]");
    if (opt.dpi <= 0 || opt.size <= 0.0 || opt.psf_sigma <= 0.0 || opt.gain <= 0.0)
        throw std::runtime_error("invalid numeric args");
    return opt;
}

std::string format_value(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << v;
    std::string s = oss.str();
    for (char &c : s) {
        if (c == '-')
            c = 'm';
        if (c == '.')
            c = 'p';
    }
    return s;
}

fs::path build_output_path(const fs::path &base, double ra, double dec, double fov, double roll) {
    const std::string stem = base.has_stem() ? base.stem().string() : "sky";
    const std::string ext = base.has_extension() ? base.extension().string() : ".bmp";
    const std::string suffix = "_ra" + format_value(ra) + "_dec" + format_value(dec) + "_fov" + format_value(fov) + "_roll" + format_value(roll);
    return base.parent_path() / (stem + suffix + ext);
}

Catalog load_catalog(const fs::path &path_in, double max_mag) {
    fs::path path = path_in;
    if (!fs::exists(path))
        throw std::runtime_error("Catalog not found: " + path.string());

    std::ifstream in(path);
    if (!in.is_open())
        throw std::runtime_error("Cannot open catalog: " + path.string());

    Catalog c;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        size_t p1 = line.find(',');
        if (p1 == std::string::npos)
            continue;
        size_t p2 = line.find(',', p1 + 1);
        if (p2 == std::string::npos)
            continue;
        size_t p3 = line.find(',', p2 + 1);
        if (p3 == std::string::npos)
            continue;
        try {
            const double ra = std::stod(line.substr(p1 + 1, p2 - p1 - 1));
            const double dec = std::stod(line.substr(p2 + 1, p3 - p2 - 1));
            const double mag = std::stod(line.substr(p3 + 1));
            if (mag <= max_mag) {
                c.ra.push_back(ra);
                c.dec.push_back(dec);
                c.mag.push_back(mag);
            }
        } catch (...) {
        }
    }
    return c;
}

std::vector<double> angular_distance_deg(const std::vector<double> &ra_deg, const std::vector<double> &dec_deg, double ra0_deg, double dec0_deg) {
    std::vector<double> out(ra_deg.size(), 0.0);
    const double ra0 = deg2rad(ra0_deg);
    const double dec0 = deg2rad(dec0_deg);
    const double sin_dec0 = std::sin(dec0);
    const double cos_dec0 = std::cos(dec0);

    for (size_t i = 0; i < ra_deg.size(); ++i) {
        const double ra = deg2rad(ra_deg[i]);
        const double dec = deg2rad(dec_deg[i]);
        const double cos_d = std::clamp(std::sin(dec) * sin_dec0 + std::cos(dec) * cos_dec0 * std::cos(ra - ra0), -1.0, 1.0);
        out[i] = rad2deg(std::acos(cos_d));
    }
    return out;
}

Projected project_and_filter(const Catalog &c, const Options &opt) {
    const double radius = opt.fov * std::sqrt(2.0) * 0.5;
    const auto dist = angular_distance_deg(c.ra, c.dec, opt.ra, opt.dec);
    Projected p;
    const double ra0 = deg2rad(opt.ra);
    const double dec0 = deg2rad(opt.dec);
    const double sin_dec0 = std::sin(dec0);
    const double cos_dec0 = std::cos(dec0);
    const double half_extent = std::tan(deg2rad(opt.fov / 2.0));
    const double theta = -deg2rad(opt.roll);
    const double cos_t = std::cos(theta);
    const double sin_t = std::sin(theta);

    for (size_t i = 0; i < c.ra.size(); ++i) {
        if (dist[i] > radius)
            continue;
        const double ra = deg2rad(c.ra[i]);
        const double dec = deg2rad(c.dec[i]);
        const double delta_ra = ra - ra0;
        const double sin_dec = std::sin(dec);
        const double cos_dec = std::cos(dec);
        const double cosc = sin_dec0 * sin_dec + cos_dec0 * cos_dec * std::cos(delta_ra);
        if (cosc <= 0.0)
            continue;
        double x = (cos_dec * std::sin(delta_ra)) / cosc;
        double y = (cos_dec0 * sin_dec - sin_dec0 * cos_dec * std::cos(delta_ra)) / cosc;
        const double xr = x * cos_t - y * sin_t;
        const double yr = x * sin_t + y * cos_t;
        x = xr;
        y = yr;
        if (std::abs(x) > half_extent || std::abs(y) > half_extent)
            continue;
        p.x.push_back(x);
        p.y.push_back(y);
        p.mag.push_back(c.mag[i]);
    }
    return p;
}

std::vector<float> gaussian_kernel_1d(double sigma) {
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    const int size = radius * 2 + 1;
    std::vector<float> k(size, 0.0f);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double v = std::exp(-0.5 * (i * i) / (sigma * sigma));
        k[i + radius] = static_cast<float>(v);
        sum += v;
    }
    for (float &x : k)
        x = static_cast<float>(x / sum);
    return k;
}

std::vector<float> convolve_x(const std::vector<float> &src, int w, int h, const std::vector<float> &k) {
    const int r = static_cast<int>(k.size() / 2);
    std::vector<float> dst(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = -r; i <= r; ++i) {
                const int xx = std::clamp(x + i, 0, w - 1);
                acc += src[static_cast<size_t>(y) * w + xx] * k[i + r];
            }
            dst[static_cast<size_t>(y) * w + x] = static_cast<float>(acc);
        }
    }
    return dst;
}

std::vector<float> convolve_y(const std::vector<float> &src, int w, int h, const std::vector<float> &k) {
    const int r = static_cast<int>(k.size() / 2);
    std::vector<float> dst(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = -r; i <= r; ++i) {
                const int yy = std::clamp(y + i, 0, h - 1);
                acc += src[static_cast<size_t>(yy) * w + x] * k[i + r];
            }
            dst[static_cast<size_t>(y) * w + x] = static_cast<float>(acc);
        }
    }
    return dst;
}

std::vector<float> render_luminance(const Projected &p, const Options &opt, int w, int h) {
    std::vector<float> star_field(static_cast<size_t>(w) * h, 0.0f);
    const double half_extent = std::tan(deg2rad(opt.fov / 2.0));
    for (size_t i = 0; i < p.x.size(); ++i) {
        const double xp = (p.x[i] + half_extent) / (2.0 * half_extent) * (w - 1);
        const double yp = (half_extent - p.y[i]) / (2.0 * half_extent) * (h - 1);
        const int xi = static_cast<int>(std::lround(xp));
        const int yi = static_cast<int>(std::lround(yp));
        if (xi < 0 || xi >= w || yi < 0 || yi >= h)
            continue;
        double flux = std::pow(10.0, -0.4 * (p.mag[i] - 8.0));
        flux = std::clamp(flux, 0.03, 80.0);
        star_field[static_cast<size_t>(yi) * w + xi] += static_cast<float>(flux);
    }

    const auto kernel = gaussian_kernel_1d(opt.psf_sigma);
    const auto blur_x = convolve_x(star_field, w, h, kernel);
    const auto blur = convolve_y(blur_x, w, h, kernel);
    std::vector<float> lum(static_cast<size_t>(w) * h, 0.0f);
    for (size_t i = 0; i < lum.size(); ++i) {
        const double core = std::sqrt(std::max(0.0f, star_field[i]));
        const double signal = std::max(0.0, static_cast<double>(blur[i]) + 0.12 * core);
        const double y = 1.0 - std::exp(-opt.gain * signal);
        lum[i] = static_cast<float>(std::clamp(y, 0.0, 1.0));
    }
    return lum;
}

void save_bmp_rgb(const fs::path &path, const std::vector<float> &lum, int w, int h) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
        throw std::runtime_error("Cannot write output: " + path.string());

    const int row_stride = w * 3;
    const int row_pad = (4 - (row_stride % 4)) % 4;
    const int pixel_data_size = (row_stride + row_pad) * h;
    const int file_size = 14 + 40 + pixel_data_size;
    auto write_u16 = [&](uint16_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
    };
    auto write_u32 = [&](uint32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF));
        out.put(static_cast<char>((v >> 24) & 0xFF));
    };

    out.put('B');
    out.put('M');
    write_u32(static_cast<uint32_t>(file_size));
    write_u16(0);
    write_u16(0);
    write_u32(54);

    write_u32(40);
    write_u32(static_cast<uint32_t>(w));
    write_u32(static_cast<uint32_t>(h));
    write_u16(1);
    write_u16(24);
    write_u32(0);
    write_u32(static_cast<uint32_t>(pixel_data_size));
    write_u32(2835);
    write_u32(2835);
    write_u32(0);
    write_u32(0);

    std::vector<uint8_t> row(static_cast<size_t>(row_stride + row_pad), 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const float l = std::clamp(lum[static_cast<size_t>(y) * w + x], 0.0f, 1.0f);
            const uint8_t r = static_cast<uint8_t>(std::lround(255.0f * l * 0.95f));
            const uint8_t g = static_cast<uint8_t>(std::lround(255.0f * l * 0.97f));
            const uint8_t b = static_cast<uint8_t>(std::lround(255.0f * l));
            row[static_cast<size_t>(x) * 3 + 0] = b;
            row[static_cast<size_t>(x) * 3 + 1] = g;
            row[static_cast<size_t>(x) * 3 + 2] = r;
        }
        out.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
    }
}

int main(int argc, char *argv[]) {
    try {
        const Options opt = parse_args(argc, argv);
        const Catalog c = load_catalog(opt.catalog_path, opt.max_mag);
        const Projected p = project_and_filter(c, opt);
        const int width = std::max(64, static_cast<int>(std::lround(opt.size * opt.dpi)));
        const int height = std::max(64, static_cast<int>(std::lround(opt.size * opt.dpi)));
        const auto lum = render_luminance(p, opt, width, height);
        const fs::path out_path = build_output_path(opt.out_path, opt.ra, opt.dec, opt.fov, opt.roll);
        save_bmp_rgb(out_path, lum, width, height);
        std::cout << "Saved image: " << fs::absolute(out_path).string() << "\n";
        std::cout << "Stars rendered: " << p.mag.size() << "\n";
        std::cout << "Format: BMP\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
