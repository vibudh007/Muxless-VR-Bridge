#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace gpuLaunch {
struct SteamProcess {std::string pid;bool dgpu;};
inline std::vector<SteamProcess> steamProcesses(const std::filesystem::path&proc="/proc"){
 std::vector<SteamProcess>result;std::error_code error;
 for(const auto&entry:std::filesystem::directory_iterator(proc,error)){
  auto pid=entry.path().filename().string();if(pid.empty()||pid.find_first_not_of("0123456789")!=std::string::npos)continue;
  struct stat owner{};if(stat(entry.path().c_str(),&owner)||owner.st_uid!=getuid())continue;
  std::ifstream nameFile(entry.path()/"comm");std::string name;std::getline(nameFile,name);if(name!="steam")continue;
  std::ifstream statFile(entry.path()/"stat");std::string status;std::getline(statFile,status);auto close=status.rfind(')');if(close!=std::string::npos&&status.size()>close+2&&status[close+2]=='Z')continue;
  std::ifstream environment(entry.path()/"environ",std::ios::binary);std::string variable;bool dgpu=false;
  while(std::getline(environment,variable,'\0'))if(variable=="DRI_PRIME=1")dgpu=true;
  result.push_back({pid,dgpu});
 }
 return result;
}
inline std::string incompatible(const std::vector<SteamProcess>&processes){
 std::string ids;for(const auto&process:processes)if(!process.dgpu){if(!ids.empty())ids+=",";ids+=process.pid;}return ids;
}
inline void requireDgpuSteam(){
 auto ids=incompatible(steamProcesses());
 if(!ids.empty())throw std::runtime_error("Steam is already running without verified DRI_PRIME=1 (PID "+ids+"). Exit Steam, then start it with wmr-gpu-bridge --steam. A SteamVR launch option cannot change an existing Steam client's GPU.");
}
}
