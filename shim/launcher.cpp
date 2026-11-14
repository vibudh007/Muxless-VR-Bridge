// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>
#include "version.hpp"
#include "gpu_launch.hpp"
#include "third_party/nlohmann/json.hpp"
namespace fs = std::filesystem;
static fs::path installation() {
    char path[4096];auto n=readlink("/proc/self/exe",path,sizeof(path)-1);
    if(n<0||n>=ssize_t(sizeof(path)-1))throw std::runtime_error("Cannot locate launcher");
    path[n]=0;
    return fs::path(path).parent_path().parent_path()/"lib/wmr-gpu-bridge"/MUXLESS_VR_BRIDGE_VERSION;
}
static void activate(const fs::path& root) {
    setenv("DRI_PRIME","1",1);
    setenv("MUXLESS_MINIMAL","1",1);
    setenv("DISABLE_MUXLESS_MINIMAL","1",1);
    unsetenv("DISABLE_MUXLESS_VR_BRIDGE");
    // Disable earlier standalone registrations to prevent two bridges/repair layers.
    setenv("DISABLE_WMR_GPU_BRIDGE","1",1);
    setenv("DISABLE_STEAMVR_RUNTIME_PATCHES","1",1);
    unsetenv("DISABLE_MUXLESS_STEAMVR_PATCHES");
    auto config=root.parent_path()/"patches";
    setenv("MUXLESS_STEAMVR_PATCH_CONFIG",config.c_str(),1);
    bool enabled=false;
    for(auto name:{"descriptor-arena","shader-compat","transfer-ring"}){
        std::ifstream file(config/(std::string(name)+".enabled"));std::string value;
        std::getline(file,value);enabled|=value=="1";
    }
    if(enabled)setenv("MUXLESS_STEAMVR_PATCHES","1",1);
    else unsetenv("MUXLESS_STEAMVR_PATCHES");
}
static void startLog(char**command) {
    const char *xdg=getenv("XDG_STATE_HOME"),*home=getenv("HOME");
    if(!home)return;
    auto runs=(xdg&&*xdg?fs::path(xdg):fs::path(home)/".local/state")/"wmr-gpu-bridge/runs";
    auto now=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto dir=runs/(std::to_string(now)+"-"+std::to_string(getpid()));fs::create_directories(dir);
    nlohmann::json info={{"version",MUXLESS_VR_BRIDGE_VERSION},{"pid",getpid()},{"command",command[0]}};
    {std::ofstream file(dir/"run.json");file<<info.dump(2)<<'\n';}
    int fd=open((dir/"runtime.log").c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(fd<0)throw std::runtime_error("Cannot open session log");
    if(dup2(fd,STDERR_FILENO)<0){close(fd);throw std::runtime_error("Cannot redirect session log");}close(fd);
    std::vector<fs::path> old;
    for(auto&e:fs::directory_iterator(runs))if(e.is_directory()&&!e.is_symlink()&&fs::exists(e.path()/"run.json"))old.push_back(e.path());
    std::sort(old.begin(),old.end());
    while(old.size()>3){std::error_code ec;fs::remove_all(old.front(),ec);old.erase(old.begin());}
}
int main(int argc,char**argv){try{
    if(argc<2||!strcmp(argv[1],"--help")){
        std::puts("Muxless VR Bridge " MUXLESS_VR_BRIDGE_VERSION "\nUsage: wmr-gpu-bridge --steam [ARG...]\n       wmr-gpu-bridge --command COMMAND [ARG...]\n       wmr-gpu-bridge COMMAND [ARG...]\n       wmr-gpu-bridge --toggle-patches [help | all | 1 | 2 | 3]\n       wmr-gpu-bridge --doctor|--verify|--logs|--uninstall\nSteamVR launch option: /absolute/path/to/wmr-gpu-bridge %command%");return argc<2?2:0;
    }
    if(!strcmp(argv[1],"--version")){std::puts(MUXLESS_VR_BRIDGE_VERSION);return 0;}
    auto root=installation();
    for(auto option:{"--doctor","--verify","--logs","--uninstall","--toggle-patches","-toggle-patches"})if(!strcmp(argv[1],option)){
        std::string manager=(root/"muxless-vr-bridge-manager").string();argv[0]=const_cast<char*>(manager.c_str());execv(manager.c_str(),argv);throw std::runtime_error("Manager not installed; use the AppImage");
    }
    bool steam=!strcmp(argv[1],"--steam");
    if(steam)gpuLaunch::requireDgpuSteam();
    int first=(!strcmp(argv[1],"--command")||!strcmp(argv[1],"--"))?2:1;
    if(!steam&&first>=argc)throw std::runtime_error("Missing command");
    if(!steam&&argv[first][0]=='-')throw std::runtime_error("Unknown option; use --help");
    activate(root);
    if(steam){std::vector<char*>args{const_cast<char*>("steam")};for(int i=2;i<argc;i++)args.push_back(argv[i]);args.push_back(nullptr);execvp("steam",args.data());}
    else {startLog(argv+first);execvp(argv[first],argv+first);}
    std::perror("Cannot launch command");return 127;
}catch(const std::exception&e){std::fprintf(stderr,"Muxless VR Bridge: %s\n",e.what());return 1;}}
