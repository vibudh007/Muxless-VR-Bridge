#pragma once
#include "version.hpp"
#include <sys/file.h>
#include <fcntl.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>
static std::string bridgeUtc(){
 std::time_t now=std::time(nullptr);std::tm tm{};gmtime_r(&now,&tm);char buffer[32];std::strftime(buffer,sizeof(buffer),"%Y-%m-%dT%H:%M:%SZ",&tm);return buffer;
}
static std::string bridgeJsonString(const std::string&text){std::string out="\"";for(unsigned char c:text){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c=='\n')out+="\\n";else if(c>=32)out+=c;}return out+'"';}
static std::filesystem::path bridgeRunDirectory,bridgeLogPath;
static uint64_t bridgeLogStarted=0;
static std::string bridgeSessionKey(){
 std::string id=std::to_string(getpid());
 if(const char* owner=getenv("WMR_BRIDGE_SESSION_PID")){
  std::string candidate(owner);
  if(!candidate.empty()&&candidate.size()<=10&&candidate.find_first_not_of("0123456789")==std::string::npos){
   struct stat st{};
   const auto path="/proc/"+candidate;
   if(!stat(path.c_str(),&st)&&st.st_uid==getuid())id=candidate;
  }
 }
 std::ifstream statFile("/proc/"+id+"/stat");
 std::string line;std::getline(statFile,line);auto end=line.rfind(')');
 if(end!=std::string::npos){
  std::istringstream fields(line.substr(end+2));std::string value,start;
  for(int i=0;i<=19&&fields>>value;i++)if(i==19)start=value;
  if(!start.empty()){
   std::ifstream bootFile("/proc/sys/kernel/random/boot_id");std::string boot;std::getline(bootFile,boot);
   return "session-"+boot.substr(0,8)+"-"+id+"-"+start;
  }
 }
 return "session-standalone-"+std::to_string(getpid());
}
static FILE*bridgeOpenRunLog(){
 namespace fs=std::filesystem;
 try{
  const char*overrideDir=getenv("WMR_BRIDGE_LOG_DIR");const char*state=getenv("XDG_STATE_HOME");const char*home=getenv("HOME");
  fs::path root=overrideDir?fs::path(overrideDir):(state&&*state?fs::path(state):fs::path(home?home:"/tmp")/".local/state")/"wmr-gpu-bridge";
  fs::path runs=root/"runs";fs::create_directories(runs);
  int fd=open((runs/".rotation.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
  if(fd<0)return stderr;
  if(flock(fd,LOCK_EX)){close(fd);return stderr;}
  struct Lock {int fd;~Lock(){flock(fd,LOCK_UN);close(fd);}} held{fd};
  bridgeRunDirectory=runs/bridgeSessionKey();fs::create_directories(bridgeRunDirectory);
  if(!fs::exists(bridgeRunDirectory/"run.json")){std::ofstream meta(bridgeRunDirectory/"run.json");meta<<"{\"schema\":1,\"started_utc\":"<<bridgeJsonString(bridgeUtc())<<",\"session\":"<<bridgeJsonString(bridgeRunDirectory.filename().string())<<"}\n";}
  std::vector<fs::path>managed;
  for(const auto&entry:fs::directory_iterator(runs))if(entry.is_directory()&&!entry.is_symlink()&&entry.path().filename().string().find("session-")==0&&fs::is_regular_file(entry.path()/"run.json"))managed.push_back(entry.path());
  std::sort(managed.begin(),managed.end(),[](const fs::path&a,const fs::path&b){return fs::last_write_time(a/"run.json")>fs::last_write_time(b/"run.json");});
  for(size_t i=3;i<managed.size();i++)if(managed[i]!=bridgeRunDirectory)fs::remove_all(managed[i]);
  bridgeLogPath=bridgeRunDirectory/("compositor-"+std::to_string(getpid())+".log");
  bridgeLogStarted=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return fopen(bridgeLogPath.c_str(),"a");
 }catch(...){return stderr;}
}
static void bridgeHealth(const char*event,uint64_t frames=0,int result=0){
 if(bridgeRunDirectory.empty())return;
 try{
  auto path=bridgeRunDirectory/("component-"+std::to_string(getpid())+".json"),temp=path;temp+=".new";
  {std::ofstream status(temp);status<<"{\"schema\":1,\"pid\":"<<getpid()<<",\"started_ns\":"<<bridgeLogStarted<<",\"updated_utc\":"<<bridgeJsonString(bridgeUtc())<<",\"event\":"<<bridgeJsonString(event)<<",\"frames\":"<<frames<<",\"result\":"<<result<<"}\n";}
  std::filesystem::rename(temp,path);
 }catch(...){}
}
