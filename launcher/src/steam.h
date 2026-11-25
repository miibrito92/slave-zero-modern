// Steam library discovery (both platforms).
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Library folders listed in <steam>/steamapps/libraryfolders.vdf, plus the Steam folder itself.
inline std::vector<fs::path> SteamLibraries(const std::vector<fs::path>& steamRoots) {
    std::vector<fs::path> libs;
    std::error_code ec;
    auto add = [&](const fs::path& p) {
        if (!fs::is_directory(p / "steamapps", ec)) return;
        fs::path c = fs::weakly_canonical(p, ec);
        for (auto& l : libs)
            if (fs::weakly_canonical(l, ec) == c) return;
        libs.push_back(p);
    };
    for (const fs::path& root : steamRoots) {
        add(root);
        std::ifstream f(root / "steamapps" / "libraryfolders.vdf");
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::regex re("\"path\"\\s+\"([^\"]+)\"");
        for (std::sregex_iterator it(all.begin(), all.end(), re), end; it != end; ++it) {
            std::string p = (*it)[1].str(), un;
            for (size_t i = 0; i < p.size(); ++i) {  // VDF escapes backslashes
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                un += p[i];
            }
            add(fs::u8path(un));
        }
    }
    return libs;
}

inline bool PathIsSteamInstall(const fs::path& gameDir) {
    std::string s = gameDir.lexically_normal().generic_u8string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s.find("/steamapps/common/") != std::string::npos;
}
