#include "../shim/subprocess.hpp"
#include <cassert>
#include <cstdio>
int main(){
 auto ok=boundedCommand({"/bin/sh","-c","printf hello; exit 7"});assert(ok.first==7&&ok.second=="hello");
 auto signalled=boundedCommand({"/bin/sh","-c","kill -TERM $$"});assert(signalled.first==143);
 const auto start=std::chrono::steady_clock::now();
 auto timeout=boundedCommand({"/bin/sh","-c","sleep 10 & wait"},std::chrono::milliseconds(70));
 assert(timeout.first==124&&std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
 auto inherited=boundedCommand({"/bin/sh","-c","sleep 10 & exit 0"},std::chrono::milliseconds(70));assert(inherited.first==124);
 auto large=boundedCommand({"/usr/bin/head","-c","1200000","/dev/zero"});assert(large.first==0&&large.second.size()==1024*1024);
 auto closed=boundedCommand({"/bin/sh","-c","exec 1>&- 2>&-; sleep 10"},std::chrono::milliseconds(70));assert(closed.first==124);
 auto missing=boundedCommand({"/nonexistent/wmr-probe"});assert(missing.first==127);
 puts("PASS: exit/signal status; bounded hung/inherited-pipe probes; process-group timeout; capped output fully drained; exec failure.");
}
