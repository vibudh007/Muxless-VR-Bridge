#pragma once
#include <sstream>
#include <string>
#include <string_view>

// Shared text editing keeps unknown options/comments intact across upgrades.
namespace bridgeSettings {
inline std::string value(const std::string& text, const std::string& key,
                         const std::string& fallback = "") {
    std::string_view remaining(text);
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        auto line = remaining.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > key.size() && line[key.size()] == '=' &&
            line.compare(0, key.size(), key) == 0) {
            return std::string(line.substr(key.size() + 1));
        }
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1);
    }
    return fallback;
}
inline std::string set(const std::string& text, const std::string& key,
                       const std::string& setting, bool onlyIfMissing = false) {
    const auto prefix = key + "=";
    std::istringstream input(text);
    std::string line, result;
    bool found = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, prefix.size(), prefix) == 0) {
            if (onlyIfMissing) return text;
            if (!found) result += prefix + setting + "\n";
            found = true;
        } else result += line + "\n";
    }
    if (!found) result += prefix + setting + "\n";
    return result;
}
}
