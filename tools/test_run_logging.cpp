#include <unistd.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <string>
#include "../shim/run_logging.hpp"
int main(int argc,char**argv){if(argc!=2)return 2;setenv("WMR_BRIDGE_LOG_DIR",argv[1],1);auto owner=std::to_string(getpid());setenv("WMR_BRIDGE_SESSION_PID",owner.c_str(),1);for(int i=0;i<2;i++){pid_t child=fork();if(child==0){FILE*f=bridgeOpenRunLog();if(!f)return 3;std::fprintf(f,"test compositor %d\n",getpid());std::fflush(f);bridgeHealth("presenting",42);fclose(f);_exit(0);}int status;waitpid(child,&status,0);if(status)return 4;}return 0;}
