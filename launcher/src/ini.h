// Minimal INI editor that preserves comments, ordering and line endings.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

class Ini {
public:
    bool Load(const std::filesystem::path& p) {
        path_ = p;
        lines_.clear();
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string all = ss.str();
        crlf_ = all.find("\r\n") != std::string::npos;
        std::string line;
        for (char c : all) {
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines_.push_back(line);
                line.clear();
            } else {
                line += c;
            }
        }
        if (!line.empty()) lines_.push_back(line);
        return true;
    }

    bool Save() const { return SaveAs(path_); }

    bool SaveAs(const std::filesystem::path& p) const {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const char* nl = crlf_ ? "\r\n" : "\n";
        for (const auto& l : lines_) f << l << nl;
        return bool(f);
    }

    void SetCrlf(bool v) { crlf_ = v; }
    void SetPath(const std::filesystem::path& p) { path_ = p; }

    std::string Get(const std::string& sec, const std::string& key, const std::string& def = "") const {
        int s = FindSection(sec);
        if (s < 0) return def;
        int k = FindKey(s, key);
        if (k < 0) return def;
        const std::string& l = lines_[k];
        return Trim(l.substr(l.find('=') + 1));
    }
    int GetInt(const std::string& sec, const std::string& key, int def) const {
        std::string v = Get(sec, key);
        return v.empty() ? def : std::atoi(v.c_str());
    }
    float GetFloat(const std::string& sec, const std::string& key, float def) const {
        std::string v = Get(sec, key);
        return v.empty() ? def : (float)std::atof(v.c_str());
    }

    void Set(const std::string& sec, const std::string& key, const std::string& val) {
        int s = FindSection(sec);
        if (s < 0) {
            lines_.push_back("[" + sec + "]");
            lines_.push_back(key + "=" + val);
            return;
        }
        int k = FindKey(s, key);
        if (k >= 0) {
            lines_[k] = lines_[k].substr(0, lines_[k].find('=') + 1) + val;
            return;
        }
        // insert after the last non-empty line of the section
        int end = s + 1;
        while (end < (int)lines_.size() && !IsSection(lines_[end])) ++end;
        int at = end;
        while (at > s + 1 && Trim(lines_[at - 1]).empty()) --at;
        lines_.insert(lines_.begin() + at, key + "=" + val);
    }
    void SetInt(const std::string& sec, const std::string& key, int v) { Set(sec, key, std::to_string(v)); }
    void SetFloat(const std::string& sec, const std::string& key, float v, int decimals = 6) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
        Set(sec, key, buf);
    }

private:
    static std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r");
        return s.substr(a, b - a + 1);
    }
    static std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }
    static bool IsSection(const std::string& l) {
        std::string t = Trim(l);
        return t.size() >= 2 && t.front() == '[' && t.back() == ']';
    }
    int FindSection(const std::string& sec) const {
        std::string want = Lower(sec);
        for (int i = 0; i < (int)lines_.size(); ++i) {
            if (IsSection(lines_[i])) {
                std::string t = Trim(lines_[i]);
                if (Lower(Trim(t.substr(1, t.size() - 2))) == want) return i;
            }
        }
        return -1;
    }
    int FindKey(int sec, const std::string& key) const {
        std::string want = Lower(key);
        for (int i = sec + 1; i < (int)lines_.size() && !IsSection(lines_[i]); ++i) {
            const std::string& l = lines_[i];
            std::string t = Trim(l);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            if (Lower(Trim(l.substr(0, eq))) == want) return i;
        }
        return -1;
    }

    std::filesystem::path path_;
    std::vector<std::string> lines_;
    bool crlf_ = true;
};
