// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <sstream>
#include <string>
namespace vrProfile {
struct Profiles { int current=-1, vr=-1; std::string currentName; };
inline Profiles parse(const std::string& text) {
    Profiles result;
    std::istringstream lines(text);std::string line;
    while(std::getline(lines,line)) {
        std::istringstream row(line);unsigned id;std::string name;
        if(!(row>>id>>name)||id>65535)continue;
        const auto mark=name.find_first_of(":*");
        if(mark!=std::string::npos)name.resize(mark);
        if(name.empty())continue;
        if(line.find('*')!=std::string::npos){result.current=static_cast<int>(id);result.currentName=name;}
        if(name=="VR")result.vr=static_cast<int>(id);
    }
    return result;
}
}
