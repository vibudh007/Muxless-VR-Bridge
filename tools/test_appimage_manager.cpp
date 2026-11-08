#define main wmr_manager_main
#include "../shim/appimage/manager.cpp"
#undef main
#include <cassert>
#include <sys/prctl.h>
int main(){
 if(getpid()!=1){std::cerr<<"Run in a private PID namespace: unshare --user --map-root-user --pid --fork --mount-proc TEST_BINARY\n";return 2;}
 auto a=launchSlot("\"UserLocalConfigStore\" { \"apps\" { \"250820\" { \"LaunchOptions\" \"QT_QPA_PLATFORM=xcb %command% -vr\" } } }");
 if(!a.app||!a.found||a.value!="QT_QPA_PLATFORM=xcb %command% -vr")return 1;
 auto b=launchSlot("\"apps\" { \"250820\" { \"Other\" \"retained\" } }");if(!b.app||b.found||!b.insert)return 2;
 auto removed=removeWrapper("QT_QPA_PLATFORM=xcb \"/tmp/user path/wmr-gpu-bridge\" %command% -vr","/tmp/user path/wmr-gpu-bridge");
 if(removed!="QT_QPA_PLATFORM=xcb %command% -vr")return 3;
 if(removeWrapper("something_else %command%","/tmp/user path/wmr-gpu-bridge")!="something_else %command%")return 4;
 std::cout<<"PASS: SteamVR profile targeting, empty launch-option insertion, and wrapper removal preserve other arguments.\n";
 char folder[]="/tmp/wmr-manager-unit-XXXXXX";assert(mkdtemp(folder));fs::path root=folder;
 setenv("HOME",folder,1);setenv("XDG_CONFIG_HOME",(root/"config").c_str(),1);setenv("XDG_DATA_HOME",(root/"data").c_str(),1);setenv("XDG_STATE_HOME",(root/"state").c_str(),1);setenv("WMR_BRIDGE_PREFIX",(root/"prefix").c_str(),1);
 json report={{"checks",json::array()}};Discovery missing;
 assert(!requireOasis(report,missing)&&report["checks"][0]["level"]=="error");
 Discovery present;present.oasis=root/"Oasis";write(present.oasis/"bin/linux64/driver_oasis.so","fixture");
 assert(requireOasis(report,present));
 auto lib=prefix()/"lib/wmr-gpu-bridge/future-999";auto payload=lib/"steam.desktop";auto alias=steamDesktop();
 auto receiptFile=state()/"install-receipt.json";
 auto entry=[&](const fs::path&p){return json{{"path",p.string()},{"sha256",digest(p)}};};
 auto save=[&](json files){json r={{"schema",1},{"prefix",prefix().string()},{"manifest",manifestPath().string()},{"version","future-999"},{"files",files},{"future_additive_field",{{"opaque",true}}}};write(receiptFile,r.dump());};
 write(config()/"settings.conf","enabled=1\ncustom=retained\nperformance_logging=1\n");
 write(payload,"owned desktop");fs::create_directories(alias.parent_path());fs::create_symlink(payload,alias);
 auto link=entry(alias);link["kind"]="desktop-symlink";link["target"]=payload.string();
 save(json::array({link,entry(payload)}));fs::remove(payload); // Interrupted install: dangling owned alias.
 auto result=uninstall();assert(result["preserved_modified_files"].empty());assert(!fs::is_symlink(fs::symlink_status(alias)));
 assert(read(config()/"settings.conf").find("custom=retained")!=std::string::npos);
 assert(read(config()/"settings.conf").find("enabled=0")!=std::string::npos);
 assert(uninstall()["already_uninstalled"]==true);
 write(payload,"original");auto original=entry(payload);save(json::array({original}));write(payload,"user changed");
 result=uninstall();assert(result["preserved_modified_files"].size()==1&&read(payload)=="user changed"&&fs::exists(receiptFile));
 fs::remove(payload);result=uninstall();assert(result["preserved_modified_files"].empty());
 // A receipt never grants authority over an arbitrary manifest or symlinked directory.
 auto outside=root/"outside";write(outside/"keep","unrelated");save(json::array({entry(outside/"keep")}));
 bool rejected=false;try{uninstall();}catch(const std::exception&){rejected=true;}assert(rejected&&read(outside/"keep")=="unrelated");
 fs::create_directories(lib.parent_path());fs::remove(lib);fs::create_directory_symlink(outside,lib);
 auto escaped=entry(lib/"keep");save(json::array({escaped}));rejected=false;try{uninstall();}catch(const std::exception&){rejected=true;}assert(rejected&&fs::exists(outside/"keep"));fs::remove(lib);
 write(receiptFile,"malformed");rejected=false;try{uninstall();}catch(const std::exception&){rejected=true;}assert(rejected);
 // Interrupted replacement accepts only the previously owned hash, not any content.
 write(payload,"old owned");auto pending=entry(payload);pending["previous_sha256"]=pending["sha256"];pending["sha256"]=std::string(64,'a');save(json::array({pending}));
 assert(uninstall()["preserved_modified_files"].empty());
 // Exercise real /proc process guards; run this test in its private PID namespace.
 write(payload,"owned");save(json::array({entry(payload)}));
 auto profile=home()/".local/share/Steam/userdata/1/config/localconfig.vdf";
 fs::create_directories(home()/".local/share/Steam/steamapps");
 write(profile,"\"apps\" { \"250820\" { \"LaunchOptions\" \"\\\""+(prefix()/"bin/wmr-gpu-bridge").string()+"\\\" %command%\" } }");
 for(const char*name:{"vrserver","steam"}){
  int ready[2];assert(pipe(ready)==0);pid_t child=fork();assert(child>=0);
  if(!child){close(ready[0]);prctl(PR_SET_NAME,name);assert(::write(ready[1],"x",1)==1);for(;;)pause();}
  close(ready[1]);char ack;assert(::read(ready[0],&ack,1)==1);close(ready[0]);
  auto before=read(receiptFile);rejected=false;try{uninstall();}catch(const std::exception&){rejected=true;}
  kill(child,SIGTERM);waitpid(child,nullptr,0);assert(rejected&&read(receiptFile)==before&&fs::exists(payload));
 }
 assert(uninstall()["preserved_modified_files"].empty());
 fs::remove_all(root);
 std::cout<<"PASS: required Oasis, additive future metadata, partial/dangling install, idempotence, settings preservation, modified files, malformed/out-of-scope/symlink escape receipts, interrupted replacement and actual process guards.\n";
 return 0;
}
