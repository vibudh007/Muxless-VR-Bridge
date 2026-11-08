#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>
#include <stdexcept>
#include "version.hpp"
#include "config_text.hpp"
#include "gpu_launch.hpp"
static std::filesystem::path configDirectory(){
 if(const char*x=getenv("XDG_CONFIG_HOME"))if(*x)return std::filesystem::path(x)/"wmr-gpu-bridge";
 const char*h=getenv("HOME");if(!h)throw std::runtime_error("HOME is unset");return std::filesystem::path(h)/".config/wmr-gpu-bridge";
}
static void enable(bool value){
 auto dir=configDirectory();std::filesystem::create_directories(dir);auto path=dir/"settings.conf",temp=dir/("settings.conf.tmp-"+std::to_string(getpid()));
 std::ifstream previous(path);std::string text((std::istreambuf_iterator<char>(previous)),{});
 text=bridgeSettings::set(text,"enabled",value?"1":"0");
 {std::ofstream file(temp);file<<text;if(!file)throw std::runtime_error("Cannot save shim configuration");}
 std::filesystem::rename(temp,path);
}
int main(int argc,char**argv){try{
 if(argc<2||!strcmp(argv[1],"--help")){
  std::puts("Muxless VR Bridge " MUXLESS_VR_BRIDGE_VERSION "\nUsage: wmr-gpu-bridge COMMAND [ARG...]\nSteamVR launch option: /home/USER/.local/bin/wmr-gpu-bridge %command%\nStart the entire Steam/VR stack: wmr-gpu-bridge --steam [Steam arguments]\nOptions: --enable, --disable, --status, --gpu-status, --version\nActivation is per-user and persists until --disable. Only Vulkan clients requesting DRM display acquisition are eligible.");return argc<2?2:0;
 }
 if(!strcmp(argv[1],"--doctor")||!strcmp(argv[1],"--verify")||!strcmp(argv[1],"--logs")||!strcmp(argv[1],"--uninstall")){
  char exe[4096];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)throw std::runtime_error("Cannot locate launcher");exe[n]=0;
  auto manager=std::filesystem::path(exe).parent_path().parent_path()/"lib/wmr-gpu-bridge"/MUXLESS_VR_BRIDGE_VERSION/"muxless-vr-bridge-manager";
  std::string path=manager.string();argv[0]=const_cast<char*>(path.c_str());execv(path.c_str(),argv);throw std::runtime_error("Manager is not installed; use the AppImage");
 }
 if(!strcmp(argv[1],"--version")){std::puts(MUXLESS_VR_BRIDGE_VERSION);return 0;}
 if(!strcmp(argv[1],"--enable")||!strcmp(argv[1],"--disable")){bool value=!strcmp(argv[1],"--enable");enable(value);std::puts(value?"Enabled for future compatible Vulkan clients.":"Disabled for future compatible Vulkan clients.");return 0;}
 if(!strcmp(argv[1],"--status")){auto path=configDirectory()/"settings.conf";std::ifstream file(path);std::printf("Configuration: %s\n",path.c_str());if(file)std::fputs(std::string(std::istreambuf_iterator<char>(file),{}).c_str(),stdout);else std::puts("disabled (no configuration)");return 0;}
 if(!strcmp(argv[1],"--gpu-status")){bool ok=true;for(const auto&p:gpuLaunch::steamProcesses()){std::printf("Steam PID=%s DRI_PRIME=1=%s\n",p.pid.c_str(),p.dgpu?"yes":"NO/UNKNOWN");ok&=p.dgpu;}return ok?0:1;}
 const bool startSteam=!strcmp(argv[1],"--steam");
 if(startSteam){gpuLaunch::requireDgpuSteam();setenv("DRI_PRIME","1",1);std::vector<char*>args;args.push_back(const_cast<char*>("steam"));for(int i=2;i<argc;i++)args.push_back(argv[i]);args.push_back(nullptr);execvp("steam",args.data());throw std::runtime_error("Cannot launch Steam");}
 int first=!strcmp(argv[1],"--")?2:1;if(first>=argc)throw std::runtime_error("Missing command");
 auto configuration=configDirectory()/"settings.conf";if(!std::filesystem::exists(configuration))enable(true);
 bool active=false;{std::ifstream file(configuration);std::string line;while(std::getline(file,line))if(line=="enabled=1")active=true;}
 if(active){setenv("WMR_GPU_BRIDGE","1",1);setenv("WMR_BRIDGE_MODE","alpha",1);}else{unsetenv("WMR_GPU_BRIDGE");setenv("DISABLE_WMR_GPU_BRIDGE","1",1);}
 execvp(argv[first],argv+first);std::perror("Cannot launch command");return 127;
 }catch(const std::exception&e){std::fprintf(stderr,"wmr-gpu-bridge: %s\n",e.what());return 1;}}
