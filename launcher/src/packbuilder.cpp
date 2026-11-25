#include "packbuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "miniz.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

constexpr int kScale = 4;
constexpr int kPad = 8;  // wrapped border keeps tiling textures seamless
const char* kModel = "realesrgan-x4plus";

#ifdef _WIN32
const char* kUpscalerUrl =
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
const char* kUpscalerExe = "realesrgan-ncnn-vulkan.exe";
#else
const char* kUpscalerUrl =
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip";
const char* kUpscalerExe = "realesrgan-ncnn-vulkan";
#endif

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGBA
};

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

uint32_t Rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Decodes mip level 0 of every animation frame. Returns false for formats that are not colour images.
bool Decode(const std::vector<uint8_t>& d, std::vector<Image>& frames, uint32_t& flags) {
    if (d.size() < 16) return false;
    uint32_t w = Rd32(&d[0]), h = Rd32(&d[4]), nf = Rd32(&d[8]);
    flags = Rd32(&d[12]);
    if (!w || !h || w > 4096 || h > 4096) return false;
    int bpp = (flags & 0x200) ? 4 : (flags & 0x800) ? 1 : 2;
    size_t level0 = (size_t)w * h * bpp, stride = level0;
    if (flags & 0x8) {
        stride = 0;
        for (int i = 0; (w >> i) || (h >> i); ++i) stride += (size_t)(w >> i) * (h >> i);
        stride *= bpp;
    }
    for (uint32_t f = 0; f < std::max(1u, nf); ++f) {
        size_t off = 16 + f * stride;
        if (off + level0 > d.size()) break;
        const uint8_t* s = &d[off];
        Image im{(int)w, (int)h, std::vector<uint8_t>((size_t)w * h * 4)};
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            uint8_t* o = &im.px[i * 4];
            if (bpp == 4) {
                uint32_t p = Rd32(s + i * 4);
                o[0] = p >> 16; o[1] = p >> 8; o[2] = p; o[3] = p >> 24;
            } else if (bpp == 1) {
                o[0] = o[1] = o[2] = s[i]; o[3] = 255;
            } else {
                uint32_t p = s[i * 2] | s[i * 2 + 1] << 8;
                if (flags & 0x100) {
                    uint32_t c[4] = {(p >> 8) & 15, (p >> 4) & 15, p & 15, p >> 12};
                    for (int k = 0; k < 4; ++k) o[k] = (uint8_t)(c[k] << 4 | c[k]);
                } else if (flags & 0x80) {
                    uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
                    o[0] = r << 3 | r >> 2; o[1] = g << 3 | g >> 2; o[2] = b << 3 | b >> 2; o[3] = (p & 0x8000) ? 255 : 0;
                } else {
                    uint32_t r = p >> 11, g = (p >> 5) & 63, b = p & 31;
                    o[0] = r << 3 | r >> 2; o[1] = g << 2 | g >> 4; o[2] = b << 3 | b >> 2; o[3] = 255;
                }
            }
        }
        frames.push_back(std::move(im));
    }
    return !frames.empty();
}

// Single-channel resize: bilinear or Catmull-Rom, edges clamped.
std::vector<float> Resize(const std::vector<float>& src, int w, int h, int nw, int nh, bool cubic) {
    auto at = [&](int x, int y) { return src[(size_t)std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)]; };
    auto cr = [](float p0, float p1, float p2, float p3, float t) {
        return p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    };
    std::vector<float> out((size_t)nw * nh);
    for (int y = 0; y < nh; ++y) {
        float sy = (y + 0.5f) * h / nh - 0.5f;
        int iy = (int)std::floor(sy);
        float fy = sy - iy;
        for (int x = 0; x < nw; ++x) {
            float sx = (x + 0.5f) * w / nw - 0.5f;
            int ix = (int)std::floor(sx);
            float fx = sx - ix, v;
            if (cubic) {
                float r[4];
                for (int k = 0; k < 4; ++k)
                    r[k] = cr(at(ix - 1, iy - 1 + k), at(ix, iy - 1 + k), at(ix + 1, iy - 1 + k), at(ix + 2, iy - 1 + k), fx);
                v = cr(r[0], r[1], r[2], r[3], fy);
            } else {
                float a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * fx;
                float b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * fx;
                v = a + (b - a) * fy;
            }
            out[(size_t)y * nw + x] = v;
        }
    }
    return out;
}

bool WritePng(const fs::path& p, int w, int h, int comp, const uint8_t* data) {
    return stbi_write_png(p.u8string().c_str(), w, h, comp, data, w * comp) != 0;
}

struct Job {
    std::string out;
    std::vector<uint8_t> alpha;  // empty when opaque
    bool oneBit = false;
    int w = 0, h = 0;
};

}  // namespace

PackBuilder::~PackBuilder() {
    if (thread_.joinable()) thread_.join();
}

void PackBuilder::Start(const fs::path& gameDir) {
    if (running_) return;
    if (thread_.joinable()) thread_.join();
    gameDir_ = gameDir;
    running_ = true;
    finished_ = false;
    phase_ = 1;
    done_ = total_ = 0;
    SetStatus("Preparing...");
    thread_ = std::thread([this] { Run(); });
}

void PackBuilder::SetStatus(const std::string& s) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        status_ = s;
    }
    static auto t0 = std::chrono::steady_clock::now();
    std::ofstream f(platform::ConfigDir() / "packbuilder.log", std::ios::app);
    f << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << "s  " << s << "\n";
}

std::string PackBuilder::Status() {
    std::lock_guard<std::mutex> l(mutex_);
    return status_;
}

float PackBuilder::Progress() {
    if (phase_ == 3 && total_ > 0) {
        // the upscaler works on a folder; count its output (throttled)
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now - lastCount_ > 0.5) {
            lastCount_ = now;
            std::error_code ec;
            int n = 0;
            for (auto it = fs::directory_iterator(work_ / "out", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) ++n;
            counted_ = n;
        }
        return std::min(1.0f, (float)counted_ / total_);
    }
    if ((phase_ == 2 || phase_ == 4) && total_ > 0) return (float)done_ / total_;
    return -1.0f;
}

bool PackBuilder::TakeFinished(bool& ok, std::string& msg) {
    if (!finished_.exchange(false)) return false;
    if (thread_.joinable()) thread_.join();
    ok = ok_;
    std::lock_guard<std::mutex> l(mutex_);
    msg = result_;
    return true;
}

bool PackBuilder::EnsureUpscaler(fs::path& exe, std::string& err) {
    fs::path dir = platform::ConfigDir() / "realesrgan";
    exe = dir / kUpscalerExe;
    std::error_code ec;
    if (fs::exists(exe, ec) && fs::exists(dir / "models" / (std::string(kModel) + ".bin"), ec)) return true;
    fs::create_directories(dir, ec);
    fs::path zipPath = platform::ConfigDir() / "realesrgan.zip";
    SetStatus("Downloading Real-ESRGAN upscaler (about 45 MB)...");
    if (!platform::Download(kUpscalerUrl, zipPath, err)) return false;
    SetStatus("Unpacking upscaler...");
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.u8string().c_str(), 0)) { err = "downloaded upscaler archive is invalid"; return false; }
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
        std::string name = st.m_filename;
        if (name.find("..") != std::string::npos) continue;
        if (name.find(".mp4") != std::string::npos || name.find(".jpg") != std::string::npos) continue;
        fs::path dst = dir / fs::u8path(name);
        fs::create_directories(dst.parent_path(), ec);
        mz_zip_reader_extract_to_file(&zip, i, dst.u8string().c_str(), 0);
    }
    mz_zip_reader_end(&zip);
    fs::remove(zipPath, ec);
#ifndef _WIN32
    fs::permissions(exe, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
    if (!fs::exists(exe, ec)) { err = "upscaler missing after unpacking"; return false; }
    return true;
}

void PackBuilder::Run() {
    std::string err;
    std::error_code ec;
    auto fail = [&](const std::string& m) {
        std::lock_guard<std::mutex> l(mutex_);
        result_ = m;
        ok_ = false;
        running_ = false;
        phase_ = 0;
        finished_ = true;
    };

    fs::path upscaler;
    phase_ = 1;
    if (!EnsureUpscaler(upscaler, err)) return fail("HD textures: " + err);

    // ---- extract
    phase_ = 2;
    SetStatus("Reading game textures...");
    mz_zip_archive zip{};
    fs::path zpath = gameDir_ / "Data" / "textures.zip";
    if (!mz_zip_reader_init_file(&zip, zpath.u8string().c_str(), 0)) return fail("HD textures: cannot open Data/textures.zip");
    std::map<std::string, mz_uint> index;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        char name[512];
        mz_zip_reader_get_filename(&zip, i, name, sizeof name);
        std::string lo = Lower(name);
        if (lo.size() > 4 && lo.substr(lo.size() - 4) == ".tex") index[lo.substr(0, lo.size() - 4)] = i;
    }
    auto readEntry = [&](mz_uint i) {
        size_t n = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &n, 0);
        std::vector<uint8_t> v((uint8_t*)p, (uint8_t*)p + (p ? n : 0));
        mz_free(p);
        return v;
    };

    work_ = platform::ConfigDir() / "pack-work";
    fs::remove_all(work_, ec);
    fs::create_directories(work_ / "in", ec);
    fs::create_directories(work_ / "out", ec);
    fs::path outDir = gameDir_ / "szmodern" / "textures";
    fs::create_directories(outDir, ec);

    std::map<std::string, Job> jobs;
    total_ = (int)index.size();
    done_ = 0;
    for (auto& [base, idx] : index) {
        if (++done_ % 250 == 0) SetStatus("Reading game textures... " + std::to_string(done_) + "/" + std::to_string(total_));
        if (base.rfind("i_font", 0) == 0 || base.find("_bump") != std::string::npos) continue;
        std::vector<Image> frames;
        uint32_t flags = 0;
        if (!Decode(readEntry(idx), frames, flags) || (flags & 0x800)) continue;
        auto b1 = index.find(base + "_bump1");
        if (b1 != index.end()) {  // player mech: unshaded high-res colour map
            std::vector<Image> f2;
            uint32_t fl2;
            if (Decode(readEntry(b1->second), f2, fl2)) { frames.swap(f2); flags = fl2; }
        }
        for (size_t fi = 0; fi < frames.size(); ++fi) {
            const Image& im = frames[fi];
            Job j;
            j.out = frames.size() == 1 ? base : base + "_f" + std::to_string(fi);
            j.w = im.w;
            j.h = im.h;
            j.oneBit = (flags & 0x80) && !(flags & 0x200);
            bool hasAlpha = false;
            for (size_t i = 0; i < (size_t)im.w * im.h; ++i) hasAlpha |= im.px[i * 4 + 3] != 255;
            if (hasAlpha) {
                j.alpha.resize((size_t)im.w * im.h);
                for (size_t i = 0; i < j.alpha.size(); ++i) j.alpha[i] = im.px[i * 4 + 3];
            }
            int pw = im.w + 2 * kPad, ph = im.h + 2 * kPad;
            std::vector<uint8_t> rgb((size_t)pw * ph * 3);
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x) {
                    int sx = ((x - kPad) % im.w + im.w) % im.w, sy = ((y - kPad) % im.h + im.h) % im.h;
                    std::memcpy(&rgb[((size_t)y * pw + x) * 3], &im.px[((size_t)sy * im.w + sx) * 4], 3);
                }
            char stem[16];
            std::snprintf(stem, sizeof stem, "j%05d", (int)jobs.size());
            WritePng(work_ / "in" / (std::string(stem) + ".png"), pw, ph, 3, rgb.data());
            jobs[stem] = std::move(j);
        }
    }

    // ---- upscale
    phase_ = 3;
    total_ = (int)jobs.size();
    counted_ = 0;
    SetStatus("Upscaling " + std::to_string(jobs.size()) + " textures on the GPU...");
    int rc = platform::RunProcess({upscaler.u8string(), "-i", (work_ / "in").u8string(), "-o", (work_ / "out").u8string(), "-n",
                                   kModel, "-s", std::to_string(kScale), "-f", "png"},
                                  upscaler.parent_path(), err);
    SetStatus("upscaler exit code " + std::to_string(rc) + (err.empty() ? "" : " (" + err + ")"));
    if (rc != 0) { mz_zip_reader_end(&zip); return fail("HD textures: upscaler failed (" + (err.empty() ? std::to_string(rc) : err) + ")"); }

    // ---- finalize
    phase_ = 4;
    done_ = 0;
    SetStatus("Writing the texture pack...");
    int written = 0;
    for (auto& [stem, j] : jobs) {
        if (++done_ % 250 == 0) SetStatus("Writing the texture pack... " + std::to_string(done_) + "/" + std::to_string(total_));
        int uw, uh, comp;
        stbi_uc* up = stbi_load((work_ / "out" / (stem + ".png")).u8string().c_str(), &uw, &uh, &comp, 3);
        if (!up) continue;
        int W = j.w * kScale, H = j.h * kScale, ox = kPad * kScale, oy = kPad * kScale;
        if (uw < ox + W || uh < oy + H) { stbi_image_free(up); continue; }
        int ch = j.alpha.empty() ? 3 : 4;
        std::vector<uint8_t> img((size_t)W * H * ch);
        std::vector<float> a;
        if (!j.alpha.empty()) {
            std::vector<float> src(j.alpha.begin(), j.alpha.end());
            a = Resize(src, j.w, j.h, W, H, !j.oneBit);
        }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                uint8_t* o = &img[((size_t)y * W + x) * ch];
                std::memcpy(o, &up[((size_t)(y + oy) * uw + x + ox) * 3], 3);
                if (ch == 4) {
                    float v = a[(size_t)y * W + x];
                    o[3] = j.oneBit ? (v >= 127.5f ? 255 : 0) : (uint8_t)std::clamp(v + 0.5f, 0.0f, 255.0f);
                }
            }
        stbi_image_free(up);
        if (WritePng(outDir / (j.out + ".png"), W, H, ch, img.data())) ++written;
    }

    // normal maps from shipped height maps (*_bump2)
    int nmaps = 0;
    for (auto& [base, idx] : index) {
        if (base.size() < 7 || base.substr(base.size() - 6) != "_bump2") continue;
        std::vector<Image> fr;
        uint32_t fl;
        if (!Decode(readEntry(idx), fr, fl)) continue;
        const Image& hm = fr[0];
        std::vector<float> h0((size_t)hm.w * hm.h);
        for (size_t i = 0; i < h0.size(); ++i) h0[i] = hm.px[i * 4] / 255.0f;
        int W = hm.w * kScale, H = hm.h * kScale;
        std::vector<float> hb = Resize(h0, hm.w, hm.h, W, H, true);
        const float strength = 6.0f * kScale;
        std::vector<uint8_t> nm((size_t)W * H * 3);
        auto at = [&](int x, int y) { return hb[(size_t)((y % H + H) % H) * W + (x % W + W) % W]; };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float dx = (at(x + 1, y) - at(x - 1, y)) * 0.5f * strength, dy = (at(x, y + 1) - at(x, y - 1)) * 0.5f * strength;
                float nx = -dx, ny = -dy, nz = 1.0f, l = std::sqrt(nx * nx + ny * ny + nz * nz);
                uint8_t* o = &nm[((size_t)y * W + x) * 3];
                o[0] = (uint8_t)((nx / l * 0.5f + 0.5f) * 255.0f + 0.5f);
                o[1] = (uint8_t)((ny / l * 0.5f + 0.5f) * 255.0f + 0.5f);
                o[2] = (uint8_t)((nz / l * 0.5f + 0.5f) * 255.0f + 0.5f);
            }
        if (WritePng(outDir / (base.substr(0, base.size() - 6) + "_n.png"), W, H, 3, nm.data())) ++nmaps;
    }
    mz_zip_reader_end(&zip);
    {
        std::ofstream f(gameDir_ / "szmodern" / "pack.txt");
        f << "model=" << kModel << "\nscale=" << kScale << "\ntextures=" << written << "\nnormalmaps=" << nmaps << "\n";
    }
    fs::remove_all(work_, ec);

    std::lock_guard<std::mutex> l(mutex_);
    result_ = "HD texture pack built: " + std::to_string(written) + " textures, " + std::to_string(nmaps) + " normal maps.";
    ok_ = written > 0;
    running_ = false;
    phase_ = 0;
    finished_ = true;
}
