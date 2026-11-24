#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <set>
#include <map>
#include <iomanip>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cerrno>
#include <initializer_list>
#include <optional>
#include "../../patches/steamvr/pipewire_metadata.hpp"
#include "../../patches/steamvr/room_setup_exit.hpp"
#include "../version.hpp"
#include "../config_text.hpp"
#include "../gpu_launch.hpp"
#include "../steam_launch.hpp"
#include "../subprocess.hpp"
#include "../third_party/nlohmann/json.hpp"
#include "../third_party/picosha2/picosha2.h"
namespace fs=std::filesystem;using json=nlohmann::json;
static fs::path selfPath(){char p[4096];auto n=readlink("/proc/self/exe",p,sizeof(p)-1);if(n<0)throw std::runtime_error("Cannot locate manager");p[n]=0;return p;}
static fs::path home(){const char*h=getenv("HOME");if(!h)throw std::runtime_error("HOME unset");return h;}
static fs::path xdg(const char*key,const fs::path&fallback){const char*v=getenv(key);return v&&*v?fs::path(v):fallback;}
static fs::path config(){return xdg("XDG_CONFIG_HOME",home()/".config")/"wmr-gpu-bridge";}
static fs::path state(){return xdg("XDG_STATE_HOME",home()/".local/state")/"wmr-gpu-bridge";}
static fs::path prefix(){const char*p=getenv("WMR_BRIDGE_PREFIX");return p&&*p?fs::path(p):home()/".local";}
static fs::path manifestPath(){return xdg("XDG_DATA_HOME",prefix()/"share")/"vulkan/implicit_layer.d/wmr_gpu_bridge.json";}
static fs::path libraryDirectory(){return prefix()/"lib/wmr-gpu-bridge"/MUXLESS_VR_BRIDGE_VERSION;}
static json managedManifest(const fs::path&libdir){
 auto layer=[&](const char*name,const char*library,const char*api,const char*version,
                 const char*enable,const char*disable,const char*description){
  return json{{"name",name},{"type","GLOBAL"},{"library_path",(libdir/library).string()},
              {"api_version",api},{"implementation_version",version},{"description",description},
              {"functions",{{"vkNegotiateLoaderLayerInterfaceVersion","vkNegotiateLoaderLayerInterfaceVersion"}}},
              {"enable_environment",{{enable,"1"}}},{"disable_environment",{{disable,"1"}}}};
 };
 // The upload repair must only observe compositor submissions, not the bridge's private transfers.
 return {{"file_format_version","1.2.0"},{"layers",json::array({
  layer("VK_LAYER_MUXLESS_steamvr_patches","libsteamvr_runtime_patches.so","1.3.0","2",
        "MUXLESS_STEAMVR_PATCHES","DISABLE_MUXLESS_STEAMVR_PATCHES","Independent SteamVR repairs with operation checks"),
  layer("VK_LAYER_MUXLESS_bridge","libmuxless_vr_bridge.so","1.4.0","3",
        "V3_ENABLE_LAYER","DISABLE_MUXLESS_VR_BRIDGE","Muxless VR Bridge display transport")
 })}};
}
static std::string read(const fs::path&p){std::ifstream f(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
static json readJson(const fs::path&p){try{return json::parse(read(p));}catch(...){return json::object();}}
static void write(const fs::path&p,const std::string&data){fs::create_directories(p.parent_path());auto tmp=p;tmp+=".new-"+std::to_string(getpid());{std::ofstream f(tmp,std::ios::binary);f<<data;if(!f)throw std::runtime_error("Cannot write "+tmp.string());}fs::rename(tmp,p);}
static std::string digest(const fs::path&p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Cannot hash "+p.string());return picosha2::hash256_hex_string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());}
static void copyManaged(const fs::path&from,const fs::path&to){fs::create_directories(to.parent_path());auto tmp=to;tmp+=".new-"+std::to_string(getpid());try{fs::copy_file(from,tmp,fs::copy_options::overwrite_existing);fs::permissions(tmp,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);fs::rename(tmp,to);}catch(...){std::error_code ec;fs::remove(tmp,ec);throw;}}
static std::pair<int,std::string> command(const std::vector<std::string>&args){return boundedCommand(args);}
static std::vector<int> processes(const std::string&name,const fs::path&proc="/proc"){std::vector<int>out;std::error_code ec;for(auto&e:fs::directory_iterator(proc,ec)){auto id=e.path().filename().string();if(id.empty()||id.find_first_not_of("0123456789")!=std::string::npos)continue;struct stat st{};if(stat(e.path().c_str(),&st)||st.st_uid!=getuid())continue;if(gpuLaunch::processNamed(e.path(),name)&&!gpuLaunch::zombieProcess(e.path()))out.push_back(std::stoi(id));}return out;}
static std::vector<std::string> lex(const std::string&s){std::vector<std::string>v;size_t i=0;while(i<s.size()){if(isspace((unsigned char)s[i])){i++;continue;}if(s[i]=='/'&&i+1<s.size()&&s[i+1]=='/'){while(i<s.size()&&s[i]!='\n')i++;continue;}if(s[i]=='{'||s[i]=='}'){v.push_back(s.substr(i++,1));continue;}std::string t;if(s[i]=='"'){i++;while(i<s.size()&&s[i]!='"'){if(s[i]=='\\'&&i+1<s.size())i++;t+=s[i++];}if(i<s.size())i++;}else{while(i<s.size()&&!isspace((unsigned char)s[i])&&s[i]!='{'&&s[i]!='}')t+=s[i++];}v.push_back(t);}return v;}
static json parseVdf(const std::vector<std::string>&v,size_t&i){json o=json::object();while(i<v.size()){if(v[i]=="}"){i++;break;}auto key=v[i++];if(i>=v.size())break;if(v[i]=="{"){i++;o[key]=parseVdf(v,i);}else o[key]=v[i++];}return o;}
static json vdf(const fs::path&p){auto tokens=lex(read(p));size_t index=0;return parseVdf(tokens,index);}
static fs::path appDir(){const char*p=getenv("APPDIR");if(p&&*p&&fs::exists(fs::path(p)/"payload.json"))return p;auto self=selfPath();if(fs::exists(self.parent_path().parent_path().parent_path()/"payload.json"))return self.parent_path().parent_path().parent_path();return self.parent_path();}
static fs::path probePath(){auto bundled=appDir()/"usr/bin/muxless-vr-bridge-probe";if(fs::exists(bundled))return bundled;return selfPath().parent_path()/"muxless-vr-bridge-probe";}
struct Discovery {fs::path steamvr,oasis,openvr;std::set<fs::path>steamRoots,libraries;};
static Discovery discover(){Discovery d;auto conf=xdg("XDG_CONFIG_HOME",home()/".config");d.openvr=conf/"openvr/openvrpaths.vrpath";auto paths=readJson(d.openvr);
 if(paths.contains("runtime"))for(auto&p:paths["runtime"])if(p.is_string()&&fs::exists(fs::path(p.get<std::string>())/"bin/linux64/vrcompositor")){d.steamvr=p.get<std::string>();break;}
 if(paths.contains("external_drivers"))for(auto&p:paths["external_drivers"])if(p.is_string()&&fs::exists(fs::path(p.get<std::string>())/"bin/linux64/driver_oasis.so")){d.oasis=p.get<std::string>();break;}
 for(auto root:{home()/".local/share/Steam",home()/".steam/root",home()/".steam/steam",xdg("XDG_DATA_HOME",home()/".local/share")/"Steam"})if(fs::exists(root/"steamapps")){root=fs::weakly_canonical(root);d.steamRoots.insert(root);d.libraries.insert(root);auto libraries=vdf(root/"steamapps/libraryfolders.vdf");if(libraries.contains("libraryfolders"))for(auto it=libraries["libraryfolders"].begin();it!=libraries["libraryfolders"].end();++it){if(it.value().is_object()&&it.value().contains("path"))d.libraries.insert(it.value()["path"].get<std::string>());}}
 const std::vector<std::pair<std::string,fs::path*>> apps={{"250820",&d.steamvr},{"3824490",&d.oasis}};
 for(auto root:d.libraries)for(const auto&pair:apps)if(pair.second->empty()){auto app=vdf(root/"steamapps"/(std::string("appmanifest_")+pair.first+".acf"));if(app.contains("AppState")&&app["AppState"].contains("installdir"))*pair.second=root/"steamapps/common"/app["AppState"]["installdir"].get<std::string>();}
 if(const char*p=getenv("WMR_STEAMVR_PATH"))d.steamvr=p;
 if(const char*p=getenv("WMR_OASIS_PATH"))d.oasis=p;
 return d;
}
static void check(json&report,const std::string&name,const std::string&level,const std::string&detail){report["checks"].push_back({{"name",name},{"level",level},{"detail",detail}});}
// Headset USB/tracking drivers belong to SteamVR's driver stack, not to the
// cross-GPU display bridge. Oasis discovery is informational only.
static void reportHeadsetDriver(json&report,const Discovery&d){
 bool present=!d.oasis.empty()&&fs::exists(d.oasis/"bin/linux64/driver_oasis.so");
 if(present)check(report,"Oasis (WMR driver)","ok",d.oasis.string()+"; not a bridge dependency");
 else check(report,"headset driver","ok","Use your headset's SteamVR driver; Oasis is not required by the bridge");
}
static json doctor(){auto d=discover();json r={{"schema",1},{"version",MUXLESS_VR_BRIDGE_VERSION},{"checks",json::array()},{"paths",{{"steamvr",d.steamvr.string()},{"oasis",d.oasis.string()},{"openvr",d.openvr.string()},{"manifest",manifestPath().string()},{"configuration",(config()/"settings.conf").string()},{"logs",(state()/"runs").string()}}}};
 gpuLaunch::Plan renderGpu{};
 try{renderGpu=gpuLaunch::detect(probePath());auto wrongSteam=gpuLaunch::incompatible(gpuLaunch::steamProcesses(renderGpu));r["render_gpu"]={{"name",renderGpu.name},{"selector",renderGpu.selector},{"device_uuid",renderGpu.uuid},{"driver",renderGpu.driver},{"system_driver_discovery",renderGpu.systemDrivers},{"discovery",renderGpu.discovery}};check(r,"render GPU","ok",renderGpu.name+"; "+renderGpu.selector);check(r,"Steam GPU selection",wrongSteam.empty()?"ok":"warn",wrongSteam.empty()?"No incompatible Steam client found; launch with wmr-gpu-bridge --steam":"Steam PID(s) "+wrongSteam+" use a different or unverified GPU selection; fully exit Steam and use wmr-gpu-bridge --steam");}catch(const std::exception&e){check(r,"render GPU","error",e.what());}
 struct utsname machine{};uname(&machine);check(r,"architecture",std::string(machine.machine)=="x86_64"?"ok":"error",machine.machine);int libcMajor=0,libcMinor=0;sscanf(gnu_get_libc_version(),"%d.%d",&libcMajor,&libcMinor);check(r,"glibc",(libcMajor>2||(libcMajor==2&&libcMinor>=28))?"ok":"error",std::string(gnu_get_libc_version())+" (requires 2.28 or newer)");
 reportHeadsetDriver(r,d);std::vector<const char*>libraries={"libvulkan.so.1","libwayland-client.so.0","libdrm.so.2"};
 for(const char*library:libraries){void*handle=dlopen(library,RTLD_LAZY|RTLD_LOCAL);check(r,library,handle?"ok":"error",handle?"host library available":std::string("missing: ")+dlerror());if(handle)dlclose(handle);}
 bool steamvr=!d.steamvr.empty()&&fs::exists(d.steamvr/"bin/linux64/vrcompositor");check(r,"SteamVR",steamvr?"ok":"error",steamvr?d.steamvr.string():"Install SteamVR or set WMR_STEAMVR_PATH to its installation directory");check(r,"native Steam",d.steamRoots.empty()?"warn":"ok",d.steamRoots.empty()?"native Steam paths not found; sandboxed Steam is not validated":"library folders discovered");
 if(fs::exists(probePath())){auto output=command({probePath().string(),renderGpu.systemDrivers?"--sharing-system":"--sharing"});try{
   r["vulkan"]=json::parse(output.second);
   if(output.first||!r["vulkan"].contains("devices"))throw std::runtime_error(output.second);
   const auto&missingInstanceExtensions=r["vulkan"]["missing_instance_extensions"];
   if(!missingInstanceExtensions.empty())check(r,"SteamVR Vulkan display extensions","warn","Missing instance extensions: "+missingInstanceExtensions.dump());
   for(const auto&gpu:r["vulkan"]["devices"]){
    if(gpu.value("device_uuid",std::string())!=renderGpu.uuid)continue;
    const auto&missing=gpu["missing_extensions"];
    if(!missing.empty())check(r,"render GPU Vulkan extensions","warn","Missing device extensions: "+missing.dump());
    const bool syncFileExport=gpu.value("sync_fd_export",false);
    check(r,"render GPU sync-file export",syncFileExport?"ok":"warn",syncFileExport?"Binary SYNC_FD export advertised; frame transfer and iGPU scanout are checked with the headset DRM lease":"Binary SYNC_FD export not advertised");
    break;
   }
  }catch(...){check(r,"Vulkan probe","error",output.second.substr(0,1500));}
  if(getenv("WAYLAND_DISPLAY")){auto wayland=command({probePath().string(),"--wayland"});try{r["wayland"]=json::parse(wayland.second);unsigned leases=r["wayland"].value("drm_lease_globals",0);check(r,"Wayland DRM leasing",leases?"ok":"warn",std::to_string(leases)+" lease-device globals; headset lease is verified only during SteamVR");}catch(...){check(r,"Wayland probe","warn",wayland.second.substr(0,500));}}else check(r,"display session","warn","WAYLAND_DISPLAY absent; tested presentation path is Wayland DRM leasing");
 }else check(r,"capability probe","error","probe executable missing");
 return r;
}
static std::vector<fs::path> runs(){std::vector<fs::path>v;std::error_code ec;if(!fs::exists(state()/"runs"))return v;for(auto&e:fs::directory_iterator(state()/"runs",ec))if(e.is_directory()&&!e.is_symlink()&&fs::exists(e.path()/"run.json"))v.push_back(e.path());std::sort(v.begin(),v.end(),[](const fs::path&a,const fs::path&b){return fs::last_write_time(a/"run.json")>fs::last_write_time(b/"run.json");});return v;}
static json runtimeStatus(const fs::path&proc="/proc"){
 auto list=runs();auto candidates=processes("vrcompositor",proc);
 const auto mapping=" "+(libraryDirectory()/"libmuxless_vr_bridge.so").string()+"\n";
 bool loaded=false,unreadable=false,inactive=false;int pid=0;
 for(int candidate:candidates){
  const auto process=proc/std::to_string(candidate);
  std::ifstream file(process/"maps");
  if(!file){unreadable=true;if(!pid)pid=candidate;continue;}
  std::string maps(std::istreambuf_iterator<char>(file),{});
  if(!pid)pid=candidate;
  if(maps.find(mapping)==std::string::npos)continue;
  std::ifstream environment(process/"environ",std::ios::binary);
  if(!environment){unreadable=true;continue;}
  bool enabled=false,transport=false,output=false,disabled=false;
  for(std::string entry;std::getline(environment,entry,'\0');){
   enabled|=entry=="V3_ENABLE_LAYER=1";
   if(entry.rfind("V3_TRANSPORT=",0)==0){
    const auto mode=entry.substr(sizeof("V3_TRANSPORT=")-1);
    transport=mode=="auto"||mode=="copy"||mode=="ace"||mode=="direct"||mode=="peer";
   }
   output|=entry=="V3_OUTPUT=headset"||entry=="V3_OUTPUT=desktop";
   disabled|=entry=="DISABLE_MUXLESS_VR_BRIDGE=1";
  }
  if(environment.bad()){unreadable=true;continue;}
  if(enabled&&transport&&output&&!disabled){loaded=true;pid=candidate;break;}
  inactive=true;pid=candidate;
 }
 std::string status=loaded?"loaded":inactive?"not_activated":candidates.empty()?"not_running":unreadable?"unverified":"not_loaded";
 std::string detail=loaded?"Managed bridge mapped with the activation environment. Check the bridge startup log for scanout readiness":
  inactive?"Managed bridge mapped without the active V3 launch configuration; check SteamVR launch integration":
  candidates.empty()?"SteamVR compositor is not running; start SteamVR and retry --verify --runtime":
  unreadable?"Linux did not allow the compositor mappings or environment to be read; activation is unverified":
  "Compositor found, but the installed bridge is not mapped; check SteamVR launch integration";
 return {{"state",status},{"pid",pid},{"session",list.empty()?"":list[0].string()},{"detail",detail}};
}

static bool under(const fs::path&p,const fs::path&root){auto relative=fs::weakly_canonical(p).lexically_relative(fs::weakly_canonical(root));return !relative.empty()&&*relative.begin()!="..";}
static bool profileHelperAuthorized(const fs::path&path){
 struct stat st{};
 return !lstat(path.c_str(),&st)&&S_ISREG(st.st_mode)&&st.st_uid==0&&(st.st_mode&S_ISUID)&&!(st.st_mode&0022);
}
static bool verifyPayload(){auto root=appDir();auto payload=readJson(root/"payload.json");if(!payload.contains("files"))throw std::runtime_error("Payload manifest missing; use the AppImage to install");for(auto&file:payload["files"]){auto path=root/file["path"].get<std::string>();if(!under(path,root)||digest(path)!=file["sha256"].get<std::string>())throw std::runtime_error("Payload integrity check failed: "+path.string());}return true;}

struct LaunchSlot {size_t start=0,end=0,insert=0;std::string value;bool found=false,app=false;};
static bool vdfKey(std::string value,const char* key){for(auto&c:value)c=std::tolower(static_cast<unsigned char>(c));return value==key;}
static LaunchSlot launchSlot(const std::string&text){
 struct Token{std::string value;size_t start,end;bool brace=false;};std::vector<Token>tokens;size_t p=0;
 while(p<text.size()){
  if(isspace(static_cast<unsigned char>(text[p]))){++p;continue;}
  if(text[p]=='/'&&p+1<text.size()&&text[p+1]=='/'){while(p<text.size()&&text[p]!='\n')++p;continue;}
  size_t start=p;std::string value;bool brace=text[p]=='{'||text[p]=='}';
  if(brace)value=text.substr(p++,1);
  else if(text[p]=='"'){
   ++p;
   while(p<text.size()&&text[p]!='"'){
    if(text[p]=='\\'){
     if(++p==text.size())throw std::runtime_error("Truncated Steam profile escape");
     char c=text[p++];value+=c=='n'?'\n':c=='r'?'\r':c=='t'?'\t':c;
    }else value+=text[p++];
   }
   if(p==text.size())throw std::runtime_error("Unclosed quote in Steam profile; no launch options written");
   ++p;
  }else while(p<text.size()&&!isspace(static_cast<unsigned char>(text[p]))&&text[p]!='{'&&text[p]!='}')value+=text[p++];
  tokens.push_back({value,start,p,brace});
 }
 LaunchSlot result;size_t i=0;
 std::function<void(std::vector<std::string>)>walk=[&](std::vector<std::string>path){
  bool app=path.size()>1&&path.back()=="250820"&&vdfKey(path[path.size()-2],"apps");
  while(i<tokens.size()){
   if(tokens[i].brace){
    if(tokens[i].value!="}"||path.empty())throw std::runtime_error("Unexpected brace in Steam profile");
    if(app){result.app=true;result.insert=tokens[i].start;}
    ++i;return;
   }
   std::string key=tokens[i++].value;
   if(i==tokens.size())throw std::runtime_error("Missing Steam profile value");
   if(tokens[i].brace){
    if(tokens[i].value!="{")throw std::runtime_error("Missing Steam profile value before closing brace");
    ++i;auto child=path;child.push_back(key);walk(child);
   }else{
    if(app&&vdfKey(key,"launchoptions")){result.start=tokens[i].start;result.end=tokens[i].end;result.value=tokens[i].value;result.found=true;}
    ++i;
   }
  }
  if(!path.empty())throw std::runtime_error("Unclosed section in Steam profile; no launch options written");
 };
 walk({});return result;
}

static std::vector<fs::path> steamConfigurations(){
 std::vector<fs::path>files;std::set<fs::path>roots;std::error_code ec;
 // Removal discovers Steam profiles independently of runtime/driver JSON.
 for(auto root:{home()/".local/share/Steam",home()/".steam/root",home()/".steam/steam",xdg("XDG_DATA_HOME",home()/".local/share")/"Steam"})if(fs::exists(root/"steamapps"))roots.insert(fs::weakly_canonical(root));
 for(auto root:roots){auto userdata=root/"userdata";if(!fs::exists(userdata))continue;for(auto&e:fs::directory_iterator(userdata,ec)){auto file=e.path()/"config/localconfig.vdf";if(fs::is_regular_file(file)&&launchSlot(read(file)).app)files.push_back(file);}}
 return files;
}
static std::string removeWrapper(std::string value,const fs::path&launcher){
 for(auto token:{"\""+launcher.string()+"\"",launcher.string()}){size_t position;while((position=value.find(token))!=std::string::npos){bool left=position==0||isspace((unsigned char)value[position-1]);size_t end=position+token.size();bool right=end==value.size()||isspace((unsigned char)value[end]);if(!left||!right)break;if(end<value.size()&&value[end]==' ')end++;value.erase(position,end-position);}}
 return value;
}
static void setLaunch(const fs::path&file,const std::string&value){
 auto before=read(file),text=before;auto slot=launchSlot(text);
 if(slot.found)text.replace(slot.start,slot.end-slot.start,json(value).dump());
 else if(slot.app)text.insert(slot.insert,"\t\t\t\t\t\t\"LaunchOptions\"\t\t"+json(value).dump()+"\n\t\t\t\t\t");
 else throw std::runtime_error("SteamVR profile disappeared");
 if(launchSlot(text).value!=value)throw std::runtime_error("SteamVR launch option could not be serialized safely");
 auto mode=fs::status(file).permissions();auto temporary=file;temporary+=".new-"+std::to_string(getpid());
 try{
  {std::ofstream out(temporary,std::ios::binary);out<<text;if(!out)throw std::runtime_error("Cannot stage SteamVR launch options");}
  fs::permissions(temporary,mode);
  if(read(file)!=before)throw std::runtime_error("Steam profile changed concurrently; quit Steam and retry installation");
  fs::rename(temporary,file);
 }catch(...){std::error_code ec;fs::remove(temporary,ec);throw;}
}
static bool managerSkipSteamConfig=false;
static json configureSteamProfiles(const std::vector<fs::path>&profiles,const fs::path&launcher,bool running){
 json changes=json::array();
 for(auto file:profiles){
  auto before=launchSlot(read(file)).value;
  try{
   auto after=steamLaunch::configure(before,launcher.string());
   if(!running&&after!=before)setLaunch(file,after);
   changes.push_back({{"file",file.string()},{"before",before},{"after",after},{"applied",!running},{"note",running?"Steam is running; exit Steam and rerun --install to apply":"Existing environment, driver launcher and arguments preserved"}});
  }catch(const std::exception&e){
   changes.push_back({{"file",file.string()},{"before",before},{"after",before},{"applied",false},{"note",e.what()}});
  }
 }
 return changes;
}
static json configureSteam(){
 if(managerSkipSteamConfig)return json::array();
 return configureSteamProfiles(steamConfigurations(),prefix()/"bin/wmr-gpu-bridge",!processes("steam").empty());
}
static void verifySteamLaunch(json&report){
 auto launcher=prefix()/"bin/wmr-gpu-bridge";
 for(auto file:steamConfigurations()){
  auto value=launchSlot(read(file)).value;
  try{
   bool valid=steamLaunch::installed(value,launcher.string());
   check(report,"SteamVR launch integration ("+file.string()+")",valid?"ok":"warn",valid?"Bridge runs before the existing SteamVR launcher":"Bridge is missing or is an argument to another launcher; fully quit Steam and rerun --install. Suggested: "+steamLaunch::configure(value,launcher.string()));
  }catch(const std::exception&e){check(report,"SteamVR launch integration ("+file.string()+")","warn",e.what());}
 }
}

static bool hasOwnedLaunchOption(const fs::path&launcher){for(auto file:steamConfigurations())if(launchSlot(read(file)).value.find(launcher.string())!=std::string::npos)return true;return false;}
struct ManagerLock {int fd;ManagerLock(){fs::create_directories(state());fd=open((state()/"manager.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);if(fd<0||flock(fd,LOCK_EX))throw std::runtime_error("Cannot lock installation state");}~ManagerLock(){flock(fd,LOCK_UN);close(fd);}};

static void replaceRepairFile(const fs::path& path, const std::string& before, const std::string& after) {
 const auto mode = fs::status(path).permissions();
 auto temporary = path.string() + ".wmr-XXXXXX";
 const int fd = mkstemp(temporary.data());
 if (fd < 0) throw std::runtime_error("Cannot stage SteamVR repair in " + path.parent_path().string());
 try {
  for (size_t written = 0; written < after.size();) {
   const auto count = ::write(fd, after.data() + written, after.size() - written);
   if (count < 0 && errno == EINTR) continue;
   if (count <= 0) throw std::runtime_error("Cannot write staged SteamVR repair");
   written += static_cast<size_t>(count);
  }
  if (fchmod(fd, static_cast<mode_t>(mode)) || fsync(fd))
   throw std::runtime_error("Cannot finalize staged SteamVR repair");
  if (!fs::is_regular_file(fs::symlink_status(path)) || read(path) != before)
   throw std::runtime_error("SteamVR file changed during repair; exit Steam and retry");
  fs::rename(temporary, path);
 } catch (...) {
  close(fd);
  std::error_code ec;
  fs::remove(temporary, ec);
  throw;
 }
 close(fd);
}

struct RepairSpan {
 size_t offset;
 std::string_view stock, repaired;
};
struct SteamVRFileRepair {
 const char *command, *description, *relativePath;
 std::initializer_list<RepairSpan> spans;
};
static constexpr SteamVRFileRepair roomSetupRepair{
 "--room-setup-fix", "Room Setup assembly",
 "tools/steamvr_room_setup/linux64/steamvr_room_setup_Data/Managed/Assembly-CSharp.dll",
 {{steamvrRoomSetup::quitMethodOffset, steamvrRoomSetup::stockQuit, steamvrRoomSetup::repairedQuit}}
};
static constexpr SteamVRFileRepair pipewireAudioRepair{
 "--pipewire-audio-fix", "PipeWire audio metadata handler", "bin/linux64/vrserver",
 {{steamvrPipewire::callbackOffset, steamvrPipewire::stockCallback, steamvrPipewire::repairedCallback},
  {steamvrPipewire::unwindRangeOffset, steamvrPipewire::stockUnwindRange, steamvrPipewire::repairedUnwindRange}}
};

static bool repairLayoutMatches(const SteamVRFileRepair& repair, std::string_view bytes) {
 if (&repair == &pipewireAudioRepair)
  return bytes.substr(0, 6) == std::string_view("\x7f" "ELF\x02\x01", 6);
 if (bytes.substr(0, 2) != "MZ") return false;
 using namespace steamvrRoomSetup;
 const auto matches = [&](size_t offset, size_t size, std::string_view expected) {
  return offset <= bytes.size() && size <= bytes.size() - offset &&
         picosha2::hash256_hex_string(std::string(bytes.substr(offset, size))) == expected;
 };
 return matches(tokenTableOffset, tokenTableSize, tokenTableSha256) &&
        matches(stringTableOffset, stringTableSize, stringTableSha256);
}

static bool repairSpansMatch(const SteamVRFileRepair& repair, std::string_view bytes, bool repaired) {
 return std::all_of(repair.spans.begin(), repair.spans.end(), [&](const RepairSpan& span) {
  const auto expected = repaired ? span.repaired : span.stock;
  return span.offset <= bytes.size() && bytes.substr(span.offset, expected.size()) == expected;
 });
}

static json repairSteamVRFile(const SteamVRFileRepair& repair, const std::string& action) {
 const bool apply = action == "apply", restore = action == "restore";
 if (!apply && !restore && action != "status")
  throw std::runtime_error(std::string("Usage: ") + repair.command + " [status | apply | restore]");
 const auto runtime = discover().steamvr;
 if (runtime.empty()) throw std::runtime_error("SteamVR installation not found");
 const auto path = runtime / repair.relativePath;
 std::optional<ManagerLock> lock;
 if (apply || restore) {
  lock.emplace();
  for (const char* name : {"vrstartup", "vrmonitor", "vrserver", "vrcompositor", "vrroomsetup", "steamvr_room_setup"})
   if (!processes(name).empty()) throw std::runtime_error("Close Room Setup and SteamVR before changing this repair");
 }
 const auto properties = fs::symlink_status(path);
 if (!fs::is_regular_file(properties))
  throw std::runtime_error(std::string("Missing or unsupported ") + repair.description + ": " + path.string());
 const auto before = read(path);
 const auto hash = picosha2::hash256_hex_string(before);
 const bool compatible = repairLayoutMatches(repair, before);
 const bool stock = compatible && repairSpansMatch(repair, before, false);
 const bool repaired = compatible && repairSpansMatch(repair, before, true);
 if (action == "status")
  return {{"state", stock ? "stock" : repaired ? "repaired" : "unsupported"}, {"path", path.string()}, {"sha256", hash}};
 if (!stock && !repaired)
  throw std::runtime_error(std::string("Unsupported ") + repair.description + " instructions or metadata at " + path.string());
 if ((apply && repaired) || (restore && stock))
  return {{"state", apply ? "repaired" : "stock"}, {"path", path.string()}, {"changed", false}, {"sha256", hash}};
 auto after = before;
 for (const auto& span : repair.spans) {
  const auto source = apply ? span.stock : span.repaired;
  const auto target = apply ? span.repaired : span.stock;
  if (before.compare(span.offset, source.size(), source) != 0)
   throw std::runtime_error(std::string(repair.description) + " did not match the expected bytes");
  after.replace(span.offset, source.size(), target);
 }
 if (!repairLayoutMatches(repair, after) || !repairSpansMatch(repair, after, apply))
  throw std::runtime_error("SteamVR repair output did not match the expected instructions or metadata");
 replaceRepairFile(path, before, after);
 return {{"state", apply ? "repaired" : "stock"}, {"path", path.string()}, {"changed", true}, {"sha256", picosha2::hash256_hex_string(after)}};
}

static fs::path steamDesktop(){return xdg("XDG_DATA_HOME",prefix()/"share")/"applications/wmr-gpu-bridge-steam.desktop";}
// The legacy standalone adapter has its own receipt. Migrate only its exact,
 // receipt-owned files and keep the bridge's removal protocol unchanged.
static std::vector<fs::path> legacyShaderFiles(){
 auto dir=home()/".local/lib/wmr-steamvr-compat";
 auto receipt=dir/"receipt.json";
 auto manifest=xdg("XDG_DATA_HOME",home()/".local/share")/"vulkan/implicit_layer.d/wmr_steamvr_shader_compat.json";
 auto library=dir/"libwmr-steamvr-compat.so";
 auto present=[](const fs::path& path){return fs::symlink_status(path).type()!=fs::file_type::not_found;};
 if(!present(manifest))return {};
 auto reject=[](){throw std::runtime_error("Standalone shader adapter files are untracked or modified; resolve them before installing");};
 if(!under(dir,home()/".local/lib")||fs::is_symlink(fs::symlink_status(dir))||!fs::is_regular_file(fs::symlink_status(receipt)))reject();
 auto owned=readJson(receipt);
 if(!owned.is_object()||!owned.contains("manifest")||!owned.contains("library"))reject();
 for(const auto& item:{std::make_pair(manifest,"manifest"),std::make_pair(library,"library")}){
  if(!present(item.first))continue;
  if(!fs::is_regular_file(fs::symlink_status(item.first)))reject();
  auto hash=digest(item.first);
  if(hash!=owned.value(item.second,"")&&hash!=owned.value(std::string("previous_")+item.second,""))reject();
 }
 return {library,manifest,receipt};
}
static bool ownedFile(const fs::path& p,fs::file_status status,const json& f){
  bool owned=false;
  if(fs::is_symlink(status)){
   if(f.value("kind","")=="desktop-symlink"){
    auto target=fs::read_symlink(p);bool current=target==fs::path(f["target"].get<std::string>());bool previous=f.contains("previous_target")&&target==fs::path(f["previous_target"].get<std::string>());
    if(current||previous)owned=!fs::exists(p)||digest(p)==f[current?"sha256":"previous_sha256"].get<std::string>();
   }
  }else if(fs::is_regular_file(status)){auto hash=digest(p);owned=hash==f["sha256"].get<std::string>()||hash==f.value("previous_sha256","");}
 return owned;
}
struct Patch {const char*id;const char*name;const char*file;const char*purpose;};
static const Patch patches[]={
 {"001","Descriptor cache ownership","descriptor-arena.enabled","Prevents descriptor-pool exhaustion; owns, frees and reclaims backing allocations."},
 {"002","Shader/layout compatibility","shader-compat.enabled","Repairs the recognized incompatible SteamVR shader and layout when present."},
 {"006","Upload staging retirement","transfer-ring.enabled","Retires completed uploads safely, preventing staging-buffer exhaustion and related compositor crashes."}
};
static fs::path patchDirectory(){return prefix()/"lib/wmr-gpu-bridge/patches";}
static bool patchOn(const Patch&p){auto text=read(patchDirectory()/p.file);return text=="1\n"||text=="1";}
static void patchHelp(){
 const char*rule="------------------------------------------------------------------------";
 std::cout<<rule<<"\nSteamVR patches (independent of the display bridge)\n"<<rule<<"\n\n"
          <<"Usage: --toggle-patches [help | all | 1 | 2 | 3]\n\n"
          <<"Restart SteamVR to apply changes.\n\n";
 for(size_t i=0;i<sizeof(patches)/sizeof(patches[0]);++i){
  const auto&p=patches[i];
  std::cout<<" "<<i+1<<": "<<p.name<<" ["<<(patchOn(p)?"selected":"not selected")<<"]\n"
           <<"   -"<<p.purpose<<"\n\n";
 }
 std::cout<<"Disabling a required repair can reintroduce crashes. No SteamVR files are edited.\n\n"
          <<" all\n   -toggle between enabling and disabling every patch.\n";
}
static json togglePatches(std::string target){
 if(target.rfind("SVR-",0)==0)target.erase(0,4);
 if(target=="1")target="001";else if(target=="2")target="002";else if(target=="3")target="006";
 if(target!="all"&&std::none_of(std::begin(patches),std::end(patches),[&](const Patch&p){return target==p.id;}))throw std::runtime_error("Unknown patch ID; use --toggle-patches help");
 ManagerLock lock;auto receiptFile=state()/"install-receipt.json";auto receipt=readJson(receiptFile);
 if(!receipt.contains("files"))throw std::runtime_error("Install the AppImage before changing patches");
 if(receipt.value("prefix","")!=prefix().string())throw std::runtime_error("Installation prefix differs from receipt");
 bool allOn=std::all_of(std::begin(patches),std::end(patches),[](const Patch&p){return patchOn(p);});
 json changes=json::array();
 for(const auto&p:patches)if(target=="all"||target==p.id){
  auto path=patchDirectory()/p.file;auto found=std::find_if(receipt["files"].begin(),receipt["files"].end(),[&](const json&f){return f.value("path","")==path.string();});
  if(found==receipt["files"].end()||!fs::is_regular_file(fs::symlink_status(path))||!ownedFile(path,fs::symlink_status(path),*found))throw std::runtime_error("Patch setting is missing, modified or not owned: "+path.string());
 }
 for(const auto&p:patches)if(target=="all"||target==p.id){
  auto path=patchDirectory()/p.file;bool enabled=target=="all"?!allOn:!patchOn(p);std::string text=enabled?"1\n":"0\n";
  auto found=std::find_if(receipt["files"].begin(),receipt["files"].end(),[&](const json&f){return f.value("path","")==path.string();});
  (*found)["previous_sha256"]=(*found)["sha256"];(*found)["sha256"]=picosha2::hash256_hex_string(text);
  write(receiptFile,receipt.dump(2)+"\n");write(path,text);found->erase("previous_sha256");
  write(receiptFile,receipt.dump(2)+"\n");changes.push_back({{"id",p.id},{"name",p.name},{"selected",enabled}});
 }
 return {{"patches",changes},{"restart_required",true},{"note","Changes take effect on the next SteamVR launch; patch-specific compatibility checks remain enforced."}};
}
static json install(){verifyPayload();ManagerLock lock;auto preflight=doctor();for(const auto&c:preflight["checks"])if(c["level"]=="error")throw std::runtime_error("Preflight: "+c["name"].get<std::string>()+": "+c["detail"].get<std::string>()+"; run --doctor for the full report");if(!processes("vrcompositor").empty()||!processes("vrserver").empty())throw std::runtime_error("Close SteamVR before installing/upgrading");
 legacyShaderFiles();
 auto root=appDir(),libdir=libraryDirectory();json receipt=readJson(state()/"install-receipt.json");if(!receipt.contains("files"))receipt["files"]=json::array();std::map<std::string,json>files;for(auto&f:receipt["files"])files[f["path"].get<std::string>()]=f;
 auto existing=readJson(manifestPath());
 if(fs::exists(manifestPath())){
  json layers=existing.contains("layers")?existing["layers"]:json::array();
  if(existing.contains("layer"))layers.push_back(existing["layer"]);
  if(!layers.is_array()||layers.empty())throw std::runtime_error("Existing manifest is malformed; refusing overwrite");
  for(const auto&layer:layers){
   auto name=layer.value("name","");
   if(name!="VK_LAYER_WMR_gpu_bridge"&&name!="VK_LAYER_MUXLESS_bridge"&&name!="VK_LAYER_MUXLESS_steamvr_patches")throw std::runtime_error("Manifest is owned by another layer");
   fs::path previous=layer.value("library_path","");
   if(!under(previous,prefix()/"lib/wmr-gpu-bridge"))throw std::runtime_error("Existing layer points outside the managed prefix");
   if(fs::is_regular_file(previous))files[previous.string()]={{"path",previous.string()},{"sha256",digest(previous)}};
  }
 }
 for(const auto&p:patches){
  auto path=patchDirectory()/p.file;auto previous=files.find(path.string());
  auto status=fs::symlink_status(path);
  if(status.type()!=fs::file_type::not_found&&
     (previous==files.end()||!ownedFile(path,status,previous->second)))
   throw std::runtime_error("Patch setting is not owned or was modified: "+path.string());
 }
 auto desktop=steamDesktop();
 auto oldShortcut=files.find(desktop.string());
 if(oldShortcut!=files.end()){
  auto status=fs::symlink_status(desktop);
  if(status.type()==fs::file_type::not_found)files.erase(oldShortcut);
  else if(ownedFile(desktop,status,oldShortcut->second)){
   fs::remove(desktop);
   files.erase(oldShortcut);
  }
 }
 // Write-ahead ownership: an interrupted copy/upgrade remains removable.
 auto persistReceipt=[&](){
  json current={{"schema",1},{"removal_protocol",1},{"version",MUXLESS_VR_BRIDGE_VERSION},{"prefix",prefix().string()},{"manifest",manifestPath().string()},{"files",json::array()}};
  if(files.count(desktop.string()))current["files"].push_back(files.at(desktop.string()));
  for(auto&f:files)if(f.first!=desktop.string())current["files"].push_back(f.second);
  write(state()/"install-receipt.json",current.dump(2)+"\n");
 };
 auto record=[&](const fs::path&path,const std::string&hash){
  json owned={{"path",path.string()},{"sha256",hash}};auto old=files.find(path.string());
  if(old!=files.end()&&old->second.value("sha256","")!=hash)owned["previous_sha256"]=old->second["sha256"];
  files[path.string()]=owned;persistReceipt();
 };
 std::vector<std::pair<fs::path,fs::path>>copies={{root/"usr/lib/libmuxless_vr_bridge.so",libdir/"libmuxless_vr_bridge.so"},{root/"usr/lib/libsteamvr_runtime_patches.so",libdir/"libsteamvr_runtime_patches.so"},{root/"usr/bin/wmr-gpu-bridge",prefix()/"bin/wmr-gpu-bridge"},{root/"usr/bin/muxless-vr-bridge-manager",libdir/"muxless-vr-bridge-manager"},{root/"usr/bin/muxless-vr-bridge-probe",libdir/"muxless-vr-bridge-probe"},{root/"usr/bin/muxless-vr-profile",libdir/"muxless-vr-profile"}};
 for(auto&pair:copies){
  const auto hash=digest(pair.first);
  const bool keepAuthorized=pair.second.filename()=="muxless-vr-profile"&&profileHelperAuthorized(pair.second)&&digest(pair.second)==hash;
  record(pair.second,hash);
  if(!keepAuthorized)copyManaged(pair.first,pair.second);
 }
 auto manifest=managedManifest(libdir);
 for(const auto&p:patches){
  auto path=patchDirectory()/p.file;
  if(!fs::exists(path)){std::string text="0\n";record(path,picosha2::hash256_hex_string(text));write(path,text);}
 }
 // Diagnostic observer is never activated by release toggles.
 auto observer=patchDirectory()/"descriptor-observer.enabled";std::string observerText="0\n";
 record(observer,picosha2::hash256_hex_string(observerText));write(observer,observerText);
 auto manifestText=manifest.dump(2)+"\n";record(manifestPath(),picosha2::hash256_hex_string(manifestText));write(manifestPath(),manifestText);
 auto configFile=config()/"settings.conf";if(fs::exists(configFile)&&!fs::exists(state()/"configuration-before-appimage.conf"))write(state()/"configuration-before-appimage.conf",read(configFile));auto settings=read(configFile);settings=bridgeSettings::set(settings,"enabled","1",true);write(configFile,settings);
 auto launchChanges=configureSteam();
 for(auto it=files.begin();it!=files.end();){
  fs::path path=it->first;
  if(under(path,prefix()/"lib/wmr-gpu-bridge")&&!under(path,libdir)&&!under(path,patchDirectory())){
   auto status=fs::symlink_status(path);
   if(status.type()==fs::file_type::not_found||ownedFile(path,status,it->second)){
    if(status.type()!=fs::file_type::not_found)fs::remove(path);
    std::error_code ec;fs::remove(path.parent_path(),ec);it=files.erase(it);continue;
   }
  }
  ++it;
 }
 for(auto&f:files){f.second.erase("previous_sha256");f.second.erase("previous_target");}receipt={{"launch_options",launchChanges},{"schema",1},{"version",MUXLESS_VR_BRIDGE_VERSION},{"prefix",prefix().string()},{"manifest",manifestPath().string()},{"files",json::array()}};// Compatibility ABI: unlink desktop alias before removing its payload.
 receipt["removal_protocol"]=1;
 if(files.count(desktop.string()))receipt["files"].push_back(files.at(desktop.string()));
 for(auto&f:files)if(f.first!=desktop.string())receipt["files"].push_back(f.second);
 write(state()/"install-receipt.json",receipt.dump(2)+"\n");
 for(const auto& path:legacyShaderFiles())fs::remove(path);
 return {{"installed",true},{"version",MUXLESS_VR_BRIDGE_VERSION},{"launch_option","\""+(prefix()/"bin/wmr-gpu-bridge").string()+"\" %command%"},{"launch_options",launchChanges},{"note",processes("steam").empty()?"per-user activation installed; review launch_options for changes or manual configuration notes":"per-user activation installed; Steam is running so launch options were not edited. Fully quit Steam and rerun --install to preserve and wrap its existing driver launcher"}};
}
static json verify(){json report={{"checks",json::array()},{"runtime",runtimeStatus()}};auto receipt=readJson(state()/"install-receipt.json");if(!receipt.contains("files")){check(report,"installation receipt","error","missing; install using AppImage");return report;}for(auto&f:receipt["files"]){fs::path path=f["path"].get<std::string>();bool ok=fs::is_regular_file(path)&&digest(path)==f["sha256"].get<std::string>();check(report,path.string(),ok?"ok":"error",ok?"checksum matches":"missing or changed");}auto manifest=readJson(manifestPath());json layers=manifest.value("layers",json::array());if(manifest.contains("layer"))layers.push_back(manifest["layer"]);
 const bool correctRegistration=manifest==managedManifest(libraryDirectory());
 check(report,"Vulkan registration",correctRegistration?"ok":"error",correctRegistration?
       "Bridge activation and repair layer order match the package":"Managed layer registration differs from this package; rerun --install");
 for(const auto&layer:layers){auto library=layer.value("library_path","");void*handle=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL);bool ok=handle&&dlsym(handle,"vkNegotiateLoaderLayerInterfaceVersion");check(report,layer.value("name","layer"),ok?"ok":"error",ok?"library dependencies resolve and loader entry point exists":(handle?"entry point missing":dlerror()));if(handle)dlclose(handle);}
 const auto helper=libraryDirectory()/"muxless-vr-profile";
 const bool authorized=profileHelperAuthorized(helper);
 check(report,"VR workload profile helper",authorized?"ok":"warn",authorized?
       "Authorized for driver-advertised VR mode":"Optional helper authorization is missing; see runtime/README.md");
 verifySteamLaunch(report);return report;}
// Removal protocol 1: schema-1 receipts remain readable by legacy AppImages.
// New metadata is additive; paths and receipt order are part of the ABI.
static bool lexicallyUnder(const fs::path&p,const fs::path&root){
 if(!p.is_absolute()||p!=p.lexically_normal())return false;
 auto relative=p.lexically_relative(root.lexically_normal());
 return !relative.empty()&&relative!="."&&*relative.begin()!="..";
}
static void validateRemoval(const json&receipt,const fs::path&managedPrefix){
 if(!managedPrefix.is_absolute()||managedPrefix!=managedPrefix.lexically_normal()||managedPrefix==managedPrefix.root_path())throw std::runtime_error("Invalid install prefix in receipt");
 if(!receipt.contains("files")||!receipt["files"].is_array()||receipt.value("schema",1)!=1)throw std::runtime_error("Unsupported or malformed removal receipt");
 auto data=xdg("XDG_DATA_HOME",managedPrefix/"share");
 auto manifest=data/"vulkan/implicit_layer.d/wmr_gpu_bridge.json";
 auto desktop=data/"applications/wmr-gpu-bridge-steam.desktop";
 auto lib=managedPrefix/"lib/wmr-gpu-bridge";
 for(const auto&f:receipt["files"]){
  if(!f.is_object()||!f.contains("path")||!f["path"].is_string()||!f.contains("sha256")||!f["sha256"].is_string())throw std::runtime_error("Malformed owned file in receipt");
  fs::path p=f["path"].get<std::string>();auto hash=f["sha256"].get<std::string>();
  if(hash.size()!=64||hash.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("Invalid owned checksum in receipt");
  if(f.contains("previous_sha256")){if(!f["previous_sha256"].is_string())throw std::runtime_error("Invalid previous checksum");auto previous=f["previous_sha256"].get<std::string>();if(previous.size()!=64||previous.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("Invalid previous checksum");}
  bool payload=lexicallyUnder(p,lib)&&under(p.parent_path(),lib);
  bool fixed=p==managedPrefix/"bin/wmr-gpu-bridge"||p==manifest||p==desktop;
  if(!payload&&!fixed)throw std::runtime_error("Receipt contains an out-of-scope path");
  if(f.contains("previous_target")&&(!f["previous_target"].is_string()||!lexicallyUnder(fs::path(f["previous_target"].get<std::string>()),lib)))throw std::runtime_error("Invalid previous desktop alias");
  if(f.value("kind","")=="desktop-symlink"){
   if(p!=desktop||!f.contains("target")||!f["target"].is_string()||!lexicallyUnder(fs::path(f["target"].get<std::string>()),lib))throw std::runtime_error("Invalid desktop alias in receipt");
  }
 }
 if(receipt.value("manifest",manifest.string())!=manifest.string())throw std::runtime_error("Receipt manifest does not match managed registration");
}
static json uninstall(){
 ManagerLock lock;
 if(!processes("vrcompositor").empty()||!processes("vrserver").empty())throw std::runtime_error("Close SteamVR before uninstalling");
 auto receiptFile=state()/"install-receipt.json";auto receipt=readJson(receiptFile);
 if(!fs::exists(receiptFile)){
  if(fs::symlink_status(manifestPath()).type()!=fs::file_type::not_found||fs::symlink_status(prefix()/"bin/wmr-gpu-bridge").type()!=fs::file_type::not_found)throw std::runtime_error("No install receipt; refusing untracked removal");
  return {{"removed",json::array()},{"preserved_modified_files",json::array()},{"already_uninstalled",true}};
 }
 auto managedPrefix=fs::path(receipt.value("prefix",prefix().string()));validateRemoval(receipt,managedPrefix);
 if(!processes("steam").empty()&&hasOwnedLaunchOption(managedPrefix/"bin/wmr-gpu-bridge"))throw std::runtime_error("Close Steam to safely remove its shim launch option, then retry uninstall");
 // Disable first. Preserve diagnostic/user settings and never run an executable
 // named by a receipt. No GPU probe, payload or Oasis is needed for removal.
 auto configFile=config()/"settings.conf";write(configFile,bridgeSettings::set(read(configFile),"enabled","0"));
 for(auto file:steamConfigurations()){auto slot=launchSlot(read(file));auto cleaned=removeWrapper(slot.value,managedPrefix/"bin/wmr-gpu-bridge");if(cleaned!=slot.value)setLaunch(file,cleaned);}
 json kept=json::array(),removed=json::array();
 for(auto&f:receipt["files"]){
  fs::path p=f["path"].get<std::string>();auto status=fs::symlink_status(p);if(status.type()==fs::file_type::not_found)continue;
  if(ownedFile(p,status,f)){fs::remove(p);removed.push_back(p.string());std::error_code ec;if(lexicallyUnder(p,managedPrefix/"lib/wmr-gpu-bridge"))fs::remove(p.parent_path(),ec);}
  else kept.push_back(p.string());
 }
 if(kept.empty())fs::remove(receiptFile);
 return {{"removed",removed},{"preserved_modified_files",kept},{"logs_preserved",(state()/"runs").string()},{"removal_protocol",1},{"note","Shim disabled and launch integration removed; modified files and logs preserved. Other Steam arguments, drivers, calibration, udev and Bluetooth untouched."}};
}
static void print(const json&r,bool machine){if(machine){std::cout<<r.dump(2)<<"\n";return;}if(r.contains("checks"))for(auto&c:r["checks"])std::cout<<"["<<c["level"].get<std::string>()<<"] "<<c["name"].get<std::string>()<<": "<<c["detail"].get<std::string>()<<"\n";if(r.contains("paths"))std::cout<<"Paths:\n"<<r["paths"].dump(2)<<"\n";if(r.contains("runtime"))std::cout<<"Runtime:\n"<<r["runtime"].dump(2)<<"\n";if(!r.contains("checks"))std::cout<<r.dump(2)<<"\n";}
static void logs(bool follow){fs::path selected;uintmax_t offset=0;for(;;){auto list=runs();fs::path latest;if(!list.empty()){fs::file_time_type time=fs::file_time_type::min();for(auto&e:fs::directory_iterator(list[0]))if(e.path().extension()==".log"&&fs::last_write_time(e.path())>time){latest=e.path();time=fs::last_write_time(e.path());}}if(!latest.empty()){if(latest!=selected){selected=latest;offset=0;std::cout<<"--- "<<selected<<" ---\n";}auto size=fs::file_size(selected);if(size<offset)offset=0;std::ifstream f(selected);f.seekg(offset);std::string chunk(std::istreambuf_iterator<char>(f),{});std::cout<<chunk<<std::flush;offset=size;}else if(!follow){std::cout<<"No managed SteamVR runs yet.\n";}if(!follow)break;std::this_thread::sleep_for(std::chrono::milliseconds(250));}}
int main(int argc,char**argv){try{
 bool machine=false,follow=false,requireRuntime=false;std::string operation="--doctor",target="help";bool chosen=false;
 for(int i=1;i<argc;i++){
  std::string arg=argv[i];
  if(arg=="--json")machine=true;else if(arg=="--follow")follow=true;else if(arg=="--runtime")requireRuntime=true;
  else if(arg=="--no-steam-config")managerSkipSteamConfig=true;
  else {
   if(chosen)throw std::runtime_error("Supply one command; use --help");
   chosen=true;operation=arg;
   if(arg=="--toggle-patches"||arg=="-toggle-patches"){
    operation="--toggle-patches";
    if(i+1<argc&&argv[i+1][0]!='-')target=argv[++i];
   }else if(arg==roomSetupRepair.command||arg==pipewireAudioRepair.command){
    target="status";
    if(i+1<argc&&argv[i+1][0]!='-')target=argv[++i];
   }
  }
 }
 if(operation=="--version"){std::cout<<MUXLESS_VR_BRIDGE_VERSION<<"\n";return 0;}
 if(operation=="--help"){std::cout<<"Muxless VR Bridge " MUXLESS_VR_BRIDGE_VERSION "\nCommands: --install, --uninstall, --doctor [--json], --verify [--runtime], --logs [--follow]\n          --toggle-patches [help | all | 1 | 2 | 3]\n          --room-setup-fix [status | apply | restore]\n          --pipewire-audio-fix [status | apply | restore]\nThe AppImage bundles the independent SteamVR patchset.\n";return 0;}
 if(operation=="--toggle-patches"){
  if(target=="help"){patchHelp();return 0;}print(togglePatches(target),machine);return 0;
 }
 if(operation==roomSetupRepair.command){print(repairSteamVRFile(roomSetupRepair,target),machine);return 0;}
 if(operation==pipewireAudioRepair.command){print(repairSteamVRFile(pipewireAudioRepair,target),machine);return 0;}
 if(operation=="--doctor"){auto r=doctor();print(r,machine);return std::any_of(r["checks"].begin(),r["checks"].end(),[](const json&c){return c["level"]=="error";})?1:0;}
 if(operation=="--install"){auto r=install();r["verification"]=verify();print(r,machine);return std::any_of(r["verification"]["checks"].begin(),r["verification"]["checks"].end(),[](const json&c){return c["level"]=="error";})?1:0;}
 if(operation=="--verify"){auto r=verify();print(r,machine);if(std::any_of(r["checks"].begin(),r["checks"].end(),[](const json&c){return c["level"]=="error";}))return 1;return requireRuntime&&r["runtime"].value("state","")!="loaded"?2:0;}
 if(operation=="--uninstall"){print(uninstall(),machine);return 0;}
 if(operation=="--logs"){logs(follow);return 0;}
 throw std::runtime_error("Unknown command; use --help");
}catch(const std::exception&e){std::cerr<<"Muxless VR Bridge: "<<e.what()<<"\n";return 1;}}
