#include "../shim/config_text.hpp"
#include <cassert>
#include <cstdio>
int main(){
 std::string original="# keep\nenabled=0\nunlit_missing_ubo=0\nperformance_logging=1\nfuture_option=retained\n";
 auto upgrade=bridgeSettings::set(original,"enabled","1",true);
 upgrade=bridgeSettings::set(upgrade,"unlit_missing_ubo","1",true);
 assert(upgrade==original);
 auto enabled=bridgeSettings::set(original,"enabled","1");
 assert(bridgeSettings::value(enabled,"enabled")=="1");
 assert(bridgeSettings::value(enabled,"unlit_missing_ubo")=="0");
 assert(bridgeSettings::value(enabled,"future_option")=="retained");
 assert(enabled.find("# keep\n")!=std::string::npos);
 auto duplicate=bridgeSettings::set("enabled=1\nenabled=0\n","enabled","0");
 assert(duplicate=="enabled=0\n");
 assert(bridgeSettings::value("enabled=1\r\n","enabled")=="1");
 assert(bridgeSettings::value("","missing","fallback")=="fallback");
 puts("PASS: upgrades preserve opt-outs/options/comments; edits collapse duplicate keys; CRLF/defaults handled.");
}
