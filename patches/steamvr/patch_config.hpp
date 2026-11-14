#pragma once
#include <cstdlib>
#include <string>
inline std::string patchConfigDirectory() {
    if (const char *p = getenv("MUXLESS_STEAMVR_PATCH_CONFIG"))
        if (*p) return p;
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    return (xdg && *xdg ? std::string(xdg) : std::string(home ? home : "") + "/.config")
           + "/steamvr-runtime-patches";
}
