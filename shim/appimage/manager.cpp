#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <set>
#include <map>
#include <iomanip>
#include <chrono>
#include <thread>
#include <algorithm>
#include "../version.hpp"
#include "../config_text.hpp"
#include "../gpu_launch.hpp"
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
static std::string read(const fs::path&p){std::ifstream f(p,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
static json readJson(const fs::path&p){try{return json::parse(read(p));}catch(...){return json::object();}}
static void write(const fs::path&p,const std::string&data){fs::create_directories(p.parent_path());auto tmp=p;tmp+=".new-"+std::to_string(getpid());{std::ofstream f(tmp,std::ios::binary);f<<data;if(!f)throw std::runtime_error("Cannot write "+tmp.string());}fs::rename(tmp,p);}
static std::string digest(const fs::path&p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Cannot hash "+p.string());return picosha2::hash256_hex_string(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());}
static void copyManaged(const fs::path&from,const fs::path&to){fs::create_directories(to.parent_path());auto tmp=to;tmp+=".new-"+std::to_string(getpid());try{fs::copy_file(from,tmp,fs::copy_options::overwrite_existing);fs::permissions(tmp,fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec);fs::rename(tmp,to);}catch(...){std::error_code ec;fs::remove(tmp,ec);throw;}}
static std::pair<int,std::string> command(const std::vector<std::string>&args){return boundedCommand(args);}
static bool alive(int pid){return pid>0&&(kill(pid,0)==0||errno==EPERM);}
static std::vector<int> processes(const std::string&name){std::vector<int>out;std::error_code ec;for(auto&e:fs::directory_iterator("/proc",ec)){auto id=e.path().filename().string();if(id.empty()||id.find_first_not_of("0123456789")!=std::string::npos)continue;struct stat st{};if(stat(e.path().c_str(),&st)||st.st_uid!=getuid())continue;auto value=read(e.path()/"comm");if(value==name+"\n")out.push_back(std::stoi(id));}return out;}
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
 for(auto root:d.libraries)for(auto pair:{std::make_pair("250820",&d.steamvr),std::make_pair("3824490",&d.oasis)})if(pair.second->empty()){auto app=vdf(root/"steamapps"/(std::string("appmanifest_")+pair.first+".acf"));if(app.contains("AppState")&&app["AppState"].contains("installdir"))*pair.second=root/"steamapps/common"/app["AppState"]["installdir"].get<std::string>();}
 if(const char*p=getenv("WMR_STEAMVR_PATH"))d.steamvr=p;if(const char*p=getenv("WMR_OASIS_PATH"))d.oasis=p;
 return d;
}
static void check(json&report,const std::string&name,const std::string&level,const std::string&detail){report["checks"].push_back({{"name",name},{"level",level},{"detail",detail}});}
static bool requireOasis(json&report,const Discovery&d){
 bool present=!d.oasis.empty()&&fs::exists(d.oasis/"bin/linux64/driver_oasis.so");
 check(report,"Oasis",present?"ok":"error",present?d.oasis.string():"Install Oasis or set WMR_OASIS_PATH to its installation directory");
 return present;
}
static json doctor(){auto d=discover();json r={{"schema",1},{"version",MUXLESS_VR_BRIDGE_VERSION},{"checks",json::array()},{"paths",{{"steamvr",d.steamvr.string()},{"oasis",d.oasis.string()},{"openvr",d.openvr.string()},{"manifest",manifestPath().string()},{"configuration",(config()/"settings.conf").string()},{"logs",(state()/"runs").string()}}}};
 auto wrongSteam=gpuLaunch::incompatible(gpuLaunch::steamProcesses());check(r,"Steam dGPU launch",wrongSteam.empty()?"ok":"warn",wrongSteam.empty()?"No incompatible Steam client found; start Steam through wmr-gpu-bridge --steam":"Steam PID(s) "+wrongSteam+" lack DRI_PRIME=1; the Steam launch helper will refuse mixed-GPU startup. Exit Steam and use wmr-gpu-bridge --steam.");
 struct utsname machine{};uname(&machine);check(r,"architecture",std::string(machine.machine)=="x86_64"?"ok":"error",machine.machine);int libcMajor=0,libcMinor=0;sscanf(gnu_get_libc_version(),"%d.%d",&libcMajor,&libcMinor);check(r,"glibc",(libcMajor>2||(libcMajor==2&&libcMinor>=28))?"ok":"error",std::string(gnu_get_libc_version())+" (requires 2.28 or newer)");
 requireOasis(r,d);std::vector<const char*>libraries={"libvulkan.so.1","libwayland-client.so.0","libdrm.so.2"};
 for(const char*library:libraries){void*handle=dlopen(library,RTLD_LAZY|RTLD_LOCAL);check(r,library,handle?"ok":"error",handle?"host library available":std::string("missing: ")+dlerror());if(handle)dlclose(handle);}
 bool steamvr=!d.steamvr.empty()&&fs::exists(d.steamvr/"bin/linux64/vrcompositor");check(r,"SteamVR",steamvr?"ok":"error",steamvr?d.steamvr.string():"Install SteamVR or set WMR_STEAMVR_PATH to its installation directory");check(r,"native Steam",d.steamRoots.empty()?"warn":"ok",d.steamRoots.empty()?"native Steam paths not found; sandboxed Steam is not validated":"library folders discovered");
 if(fs::exists(probePath())){auto output=command({probePath().string()});try{r["vulkan"]=json::parse(output.second);if(output.first||!r["vulkan"].contains("devices"))throw std::runtime_error(output.second);unsigned hardware=0,capable=0;for(auto&gpu:r["vulkan"]["devices"]){if(gpu["type"]!=4){hardware++;if(!gpu["missing_extensions"].empty())check(r,gpu["name"].get<std::string>()+" sharing","warn","missing Vulkan extensions: "+gpu["missing_extensions"].dump());if(gpu["missing_extensions"].empty()&&(gpu["dma_buf_features"].get<unsigned>()&6)==6&&(gpu["sync_fd_features"].get<unsigned>()&3)==3)capable++;}}check(r,"Vulkan devices",hardware>=2?"ok":"warn",std::to_string(hardware)+" hardware GPUs; "+std::to_string(capable)+" advertise required sharing capabilities (real transfer still needs testing)");}catch(...){check(r,"Vulkan probe","error",output.second.substr(0,1500));}
  if(getenv("WAYLAND_DISPLAY")){auto wayland=command({probePath().string(),"--wayland"});try{r["wayland"]=json::parse(wayland.second);unsigned leases=r["wayland"].value("drm_lease_globals",0);check(r,"Wayland DRM leasing",leases?"ok":"warn",std::to_string(leases)+" lease-device globals; headset lease is verified only during SteamVR");}catch(...){check(r,"Wayland probe","warn",wayland.second.substr(0,500));}}else check(r,"display session","warn","WAYLAND_DISPLAY absent; tested presentation path is Wayland DRM leasing");
 }else check(r,"capability probe","error","probe executable missing");
 return r;
}
static std::vector<fs::path> runs(){std::vector<fs::path>v;std::error_code ec;if(!fs::exists(state()/"runs"))return v;for(auto&e:fs::directory_iterator(state()/"runs",ec))if(e.is_directory()&&!e.is_symlink()&&fs::exists(e.path()/"run.json"))v.push_back(e.path());std::sort(v.begin(),v.end(),[](const fs::path&a,const fs::path&b){return fs::last_write_time(a/"run.json")>fs::last_write_time(b/"run.json");});return v;}
static json runtimeStatus(){auto list=runs();if(list.empty())return {{"state","unverified"},{"detail","no managed SteamVR run has been recorded yet"}};json selected=json::object();fs::path component;for(auto&e:fs::directory_iterator(list[0]))if(e.path().filename().string().find("component-")==0&&e.path().extension()==".json"){auto value=readJson(e.path());if(value.value("started_ns",uint64_t(0))>selected.value("started_ns",uint64_t(0))){selected=value;component=e.path();}}
 if(selected.empty())return {{"state","unverified"},{"detail","run exists without component status"}};int pid=selected.value("pid",0);bool live=alive(pid);std::string event=selected.value("event","");auto age=std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now()-fs::last_write_time(component)).count();std::string health=!live?(event=="stopped"?"stopped":"failed"):(event=="error"?"failed":(event=="presenting"&&age<20?"presenting":"waiting"));return {{"state",health},{"pid",pid},{"event",event},{"frames",selected.value("frames",uint64_t(0))},{"heartbeat_age_seconds",age},{"session",list[0].string()},{"detail",health=="waiting"?"no recent successful present; headset may be in standby":"runtime state from layer heartbeat and process liveness"}};
}
static bool under(const fs::path&p,const fs::path&root){auto relative=fs::weakly_canonical(p).lexically_relative(fs::weakly_canonical(root));return !relative.empty()&&*relative.begin()!="..";}
static bool verifyPayload(){auto root=appDir();auto payload=readJson(root/"payload.json");if(!payload.contains("files"))throw std::runtime_error("Payload manifest missing; use the AppImage to install");for(auto&file:payload["files"]){auto path=root/file["path"].get<std::string>();if(!under(path,root)||digest(path)!=file["sha256"].get<std::string>())throw std::runtime_error("Payload integrity check failed: "+path.string());}return true;}

struct LaunchSlot {size_t start=0,end=0,insert=0;std::string value;bool found=false,app=false;};
static LaunchSlot launchSlot(const std::string&text){
 struct Token{std::string value;size_t start,end;};std::vector<Token>tokens;size_t p=0;
 while(p<text.size()){if(isspace((unsigned char)text[p])){p++;continue;}if(text[p]=='/'&&p+1<text.size()&&text[p+1]=='/'){while(p<text.size()&&text[p]!='\n')p++;continue;}size_t start=p;std::string value;if(text[p]=='{'||text[p]=='}')value=text.substr(p++,1);else if(text[p]=='"'){p++;while(p<text.size()&&text[p]!='"'){if(text[p]=='\\'&&p+1<text.size())p++;value+=text[p++];}if(p<text.size())p++;}else{while(p<text.size()&&!isspace((unsigned char)text[p])&&text[p]!='{'&&text[p]!='}')value+=text[p++];}tokens.push_back({value,start,p});}
 LaunchSlot result;size_t i=0;std::function<void(std::vector<std::string>)>walk=[&](std::vector<std::string>path){bool app=path.size()>1&&path.back()=="250820"&&path[path.size()-2]=="apps";while(i<tokens.size()){if(tokens[i].value=="}"){if(app){result.app=true;result.insert=tokens[i].start;}i++;return;}std::string key=tokens[i++].value;if(i>=tokens.size())return;if(tokens[i].value=="{"){i++;auto child=path;child.push_back(key);walk(child);}else{if(app&&key=="LaunchOptions"){result.start=tokens[i].start;result.end=tokens[i].end;result.value=tokens[i].value;result.found=true;}i++;}}};walk({});return result;
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
static void setLaunch(const fs::path&file,const std::string&value){auto text=read(file);auto slot=launchSlot(text);if(slot.found)text.replace(slot.start,slot.end-slot.start,json(value).dump());else if(slot.app)text.insert(slot.insert,"\t\t\t\t\t\t\"LaunchOptions\"\t\t"+json(value).dump()+"\n\t\t\t\t\t");else throw std::runtime_error("SteamVR profile disappeared");write(file,text);}
static bool managerSkipSteamConfig=false;
static json configureSteam(){json changes=json::array();if(managerSkipSteamConfig)return changes;if(!processes("steam").empty())return changes;auto launcher=prefix()/"bin/wmr-gpu-bridge";for(auto file:steamConfigurations()){auto slot=launchSlot(read(file));auto before=slot.value;std::string after=before;if(before.find(launcher.string())==std::string::npos){auto point=after.find("%command%");if(point==std::string::npos){if(!after.empty())continue;after="%command%";point=0;}after.insert(point,"\""+launcher.string()+"\" ");setLaunch(file,after);}changes.push_back({{"file",file.string()},{"before",before},{"after",after}});}return changes;}
static bool hasOwnedLaunchOption(const fs::path&launcher){for(auto file:steamConfigurations())if(launchSlot(read(file)).value.find(launcher.string())!=std::string::npos)return true;return false;}
struct ManagerLock {int fd;ManagerLock(){fs::create_directories(state());fd=open((state()/"manager.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);if(fd<0||flock(fd,LOCK_EX))throw std::runtime_error("Cannot lock installation state");}~ManagerLock(){flock(fd,LOCK_UN);close(fd);}};
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
static json install(){verifyPayload();ManagerLock lock;auto preflight=doctor();for(const auto&c:preflight["checks"])if(c["level"]=="error")throw std::runtime_error("Preflight: "+c["name"].get<std::string>()+": "+c["detail"].get<std::string>()+"; run --doctor for the full report");if(!processes("vrcompositor").empty()||!processes("vrserver").empty())throw std::runtime_error("Close SteamVR before installing/upgrading");
 legacyShaderFiles();
 auto root=appDir(),libdir=prefix()/"lib/wmr-gpu-bridge"/MUXLESS_VR_BRIDGE_VERSION;json receipt=readJson(state()/"install-receipt.json");if(!receipt.contains("files"))receipt["files"]=json::array();std::map<std::string,json>files;for(auto&f:receipt["files"])files[f["path"].get<std::string>()]=f;
 auto existing=readJson(manifestPath());if(fs::exists(manifestPath())&&!existing.contains("layer"))throw std::runtime_error("Existing manifest is malformed or unrecognized; refusing overwrite");if(existing.contains("layer")){if(existing["layer"].value("name","")!="VK_LAYER_WMR_gpu_bridge")throw std::runtime_error("Manifest is owned by another layer");fs::path previous=existing["layer"].value("library_path","");if(!under(previous,prefix()/"lib/wmr-gpu-bridge"))throw std::runtime_error("Existing layer points outside the managed prefix; save/resolve that installation first");if(fs::is_regular_file(previous))files[previous.string()]={{"path",previous.string()},{"sha256",digest(previous)}};write(state()/"manifest-before-appimage-install.json",existing.dump(2)+"\n");}
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
 std::vector<std::pair<fs::path,fs::path>>copies={{root/"usr/lib/libmuxless_vr_bridge.so",libdir/"libmuxless_vr_bridge.so"},{root/"usr/bin/wmr-gpu-bridge",prefix()/"bin/wmr-gpu-bridge"},{root/"usr/bin/muxless-vr-bridge-manager",libdir/"muxless-vr-bridge-manager"},{root/"usr/bin/muxless-vr-bridge-probe",libdir/"muxless-vr-bridge-probe"}};
 for(auto&pair:copies){record(pair.second,digest(pair.first));copyManaged(pair.first,pair.second);}
 json manifest={{"file_format_version","1.2.0"},{"layer",{{"name","VK_LAYER_WMR_gpu_bridge"},{"type","GLOBAL"},{"library_path",(libdir/"libmuxless_vr_bridge.so").string()},{"api_version","1.3.0"},{"implementation_version","3"},{"description","Muxless VR Bridge presentation layer"},{"functions",{{"vkGetInstanceProcAddr","MuxlessVrBridge_GetInstanceProcAddr"},{"vkGetDeviceProcAddr","MuxlessVrBridge_GetDeviceProcAddr"}}},{"disable_environment",{{"DISABLE_WMR_GPU_BRIDGE","1"}}}}}};
 auto manifestText=manifest.dump(2)+"\n";record(manifestPath(),picosha2::hash256_hex_string(manifestText));write(manifestPath(),manifestText);
 auto configFile=config()/"settings.conf";if(fs::exists(configFile)&&!fs::exists(state()/"configuration-before-appimage.conf"))write(state()/"configuration-before-appimage.conf",read(configFile));auto settings=read(configFile);settings=bridgeSettings::set(settings,"enabled","1",true);settings=bridgeSettings::set(settings,"direct_scanout","1",true);settings=bridgeSettings::set(settings,"descriptor_diagnostics","0",true);write(configFile,settings);
 auto launchChanges=configureSteam();for(auto&f:files){f.second.erase("previous_sha256");f.second.erase("previous_target");}receipt={{"launch_options",launchChanges},{"schema",1},{"version",MUXLESS_VR_BRIDGE_VERSION},{"prefix",prefix().string()},{"manifest",manifestPath().string()},{"files",json::array()}};// Compatibility ABI: unlink desktop alias before removing its payload.
 receipt["removal_protocol"]=1;
 if(files.count(desktop.string()))receipt["files"].push_back(files.at(desktop.string()));
 for(auto&f:files)if(f.first!=desktop.string())receipt["files"].push_back(f.second);
 write(state()/"install-receipt.json",receipt.dump(2)+"\n");
 for(const auto& path:legacyShaderFiles())fs::remove(path);
 return {{"installed",true},{"version",MUXLESS_VR_BRIDGE_VERSION},{"launch_option","\""+(prefix()/"bin/wmr-gpu-bridge").string()+"\" %command%"},{"note",processes("steam").empty()?"per-user activation installed; available SteamVR launch options configured without changing vendor files":"per-user activation installed; Steam is running so launch options were not edited; use the printed option"}};
}
static json verify(){json report={{"checks",json::array()},{"runtime",runtimeStatus()}};auto receipt=readJson(state()/"install-receipt.json");if(!receipt.contains("files")){check(report,"installation receipt","error","missing; install using AppImage");return report;}for(auto&f:receipt["files"]){fs::path path=f["path"].get<std::string>();bool ok=fs::is_regular_file(path)&&digest(path)==f["sha256"].get<std::string>();check(report,path.string(),ok?"ok":"error",ok?"checksum matches":"missing or changed");}auto manifest=readJson(manifestPath());if(manifest.contains("layer")){auto library=manifest["layer"].value("library_path","");void*handle=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL);bool ok=handle&&dlsym(handle,"vkNegotiateLoaderLayerInterfaceVersion");check(report,"layer loading",ok?"ok":"error",ok?"library dependencies resolve and loader entry point exists":(handle?"entry point missing":dlerror()));if(handle)dlclose(handle);}return report;}
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
int main(int argc,char**argv){try{bool machine=false,follow=false,requireRuntime=false;std::string operation="--doctor";for(int i=1;i<argc;i++){std::string arg=argv[i];if(arg=="--json")machine=true;else if(arg=="--follow")follow=true;else if(arg=="--runtime")requireRuntime=true;else if(arg=="--no-steam-config")managerSkipSteamConfig=true;else operation=arg;}
 if(operation=="--version"){std::cout<<MUXLESS_VR_BRIDGE_VERSION<<"\n";return 0;}if(operation=="--help"){std::cout<<"Muxless VR Bridge " MUXLESS_VR_BRIDGE_VERSION "\nCommands: --doctor [--json], --install, --verify [--json], --logs [--follow], --uninstall\nNo root or package-manager changes. Paths can be overridden with WMR_STEAMVR_PATH and WMR_OASIS_PATH.\n";return 0;}
 if(operation=="--doctor"){auto r=doctor();print(r,machine);return std::any_of(r["checks"].begin(),r["checks"].end(),[](const json&c){return c["level"]=="error";})?1:0;}
 if(operation=="--install"){auto r=install();r["verification"]=verify();print(r,machine);return std::any_of(r["verification"]["checks"].begin(),r["verification"]["checks"].end(),[](const json&c){return c["level"]=="error";})?1:0;}if(operation=="--verify"){auto r=verify();print(r,machine);if(std::any_of(r["checks"].begin(),r["checks"].end(),[](const json&c){return c["level"]=="error";})||r["runtime"].value("state","")=="failed")return 1;return requireRuntime&&r["runtime"].value("state","")!="presenting"?2:0;}
 if(operation=="--uninstall"){print(uninstall(),machine);return 0;}if(operation=="--logs"){logs(follow);return 0;}throw std::runtime_error("Unknown command; use --help");
 }catch(const std::exception&e){std::cerr<<"wmr-gpu-bridge: "<<e.what()<<"\n";return 1;}}
