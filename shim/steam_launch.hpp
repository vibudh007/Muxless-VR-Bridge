#pragma once
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// Preserve the user's command byte-for-byte, apart from relocating our own
// executable token. Never evaluate launch options or discard a driver wrapper.
namespace steamLaunch {
struct Token { std::string value; size_t begin, end; };
inline std::vector<Token> tokens(const std::string& text) {
    std::vector<Token> out;
    for(size_t i=0;i<text.size();) {
        if(std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        Token token{{},i,0}; char quote=0;
        while(i<text.size()) {
            char c=text[i];
            if(!quote && std::isspace(static_cast<unsigned char>(c))) break;
            if(!quote && (c=='\'' || c=='"')) { quote=c; ++i; continue; }
            if(quote && c==quote) { quote=0; ++i; continue; }
            if(c=='\\' && quote!='\'') {
                if(++i==text.size()) throw std::runtime_error("Trailing escape in SteamVR launch options");
                token.value+=text[i++]; continue;
            }
            // Do not rewrite compound shell expressions. A shell operator inside
            // quotes is plain text; substitutions are unsafe in any quote but '.
            if((!quote && std::string(";|&<>()").find(c)!=std::string::npos) ||
               (quote!='\'' && (c=='`' || (c=='$' && i+1<text.size() && text[i+1]=='('))))
                throw std::runtime_error("Compound shell launch options need manual bridge placement");
            token.value+=c; ++i;
        }
        if(quote) throw std::runtime_error("Unclosed quote in SteamVR launch options");
        token.end=i; out.push_back(std::move(token));
    }
    return out;
}
inline bool assignment(const std::string& token) {
    auto equal=token.find('=');
    if(equal==std::string::npos || !equal) return false;
    for(size_t i=0;i<equal;++i) {
        auto c=static_cast<unsigned char>(token[i]);
        if(!(std::isalpha(c)||c=='_'||(i&&std::isdigit(c)))) return false;
    }
    return true;
}
inline std::string quote(const std::string& value) {
    std::string out="\"";
    for(char c:value) { if(c=='\\'||c=='"'||c=='$'||c=='`') out+='\\'; out+=c; }
    return out+'"';
}
// Locate the actual first executable after leading environment assignments (and
// env's supported prefix options). Wrappers such as vrmonitor.sh/gamemoderun are
// executable commands, NOT arguments to insert behind.
inline size_t executable(const std::vector<Token>& list) {
    size_t i=0;
    while(i<list.size()&&assignment(list[i].value)) ++i;
    if(i<list.size()&&list[i].value=="exec") ++i;
    if(i<list.size()&&std::filesystem::path(list[i].value).filename()=="env") {
        ++i;
        bool options=true;
        while(i<list.size()) {
            const auto& t=list[i].value;
            if(options&&t=="--") { options=false; ++i; continue; }
            if(options&&(t=="-i"||t=="--ignore-environment")) { ++i; continue; }
            if(options&&(t=="-u"||t=="--unset"||t=="-C"||t=="--chdir")) {
                if(i+1>=list.size()) throw std::runtime_error("Incomplete env option in SteamVR launch options");
                i+=2; continue;
            }
            if(options&&(t.rfind("--unset=",0)==0||t.rfind("--chdir=",0)==0)) { ++i; continue; }
            if(assignment(t)) { ++i; continue; }
            if(options&&!t.empty()&&t[0]=='-')
                throw std::runtime_error("Unsupported env option; configure SteamVR bridge placement manually");
            break;
        }
    }
    if(i==list.size()) throw std::runtime_error("Missing command in SteamVR launch options");
    return i;
}
inline bool installed(const std::string& text,const std::string& launcher) {
    auto list=tokens(text);size_t commands=0,owned=0;
    for(const auto& t:list) { commands+=t.value=="%command%";owned+=t.value==launcher; }
    return commands==1&&owned==1&&list[executable(list)].value==launcher;
}
inline std::string configure(const std::string& text,const std::string& launcher) {
    if(installed(text,launcher)) return text;
    auto list=tokens(text);size_t placeholder=list.size();
    for(size_t i=0;i<list.size();++i) if(list[i].value=="%command%") {
        if(placeholder!=list.size()) throw std::runtime_error("Multiple %command% tokens; configure SteamVR bridge placement manually");
        placeholder=i;
    }
    if(placeholder==list.size()) {
        if(text.find("%command%")!=std::string::npos) throw std::runtime_error("%command% must be a separate command token");
        for(const auto& t:list) if(t.value==launcher)
            throw std::runtime_error("Bridge launch option is missing %command%");
        // Steam treats options without %command% as arguments to its default
        // executable. Preserve that meaning, including -pipewire and other flags.
        return quote(launcher)+" %command%"+(text.empty()?"":" "+text);
    }
    for(size_t i=placeholder+1;i<list.size();++i) if(list[i].value==launcher)
        throw std::runtime_error("Bridge appears in application arguments; configure its position manually");
    std::string cleaned=text;
    for(size_t i=placeholder;i-->0;) if(list[i].value==launcher) {
        size_t end=list[i].end;
        if(i+1<placeholder&&(list[i+1].value=="--command"||list[i+1].value=="--")) end=list[i+1].end;
        if(end<cleaned.size()&&cleaned[end]==' ') ++end;
        cleaned.erase(list[i].begin,end-list[i].begin);
    }
    auto remaining=tokens(cleaned);
    auto command=executable(remaining);
    cleaned.insert(remaining[command].begin,quote(launcher)+" ");
    return cleaned;
}
}
