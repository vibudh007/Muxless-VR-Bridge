// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "vr_profile.hpp"
#include "profile_protocol.hpp"
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <spawn.h>
#include <dlfcn.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <cstdio>
#include <cstdlib>
extern char **environ;
namespace vrProfile {
struct Lease {
    int socket=-1;pid_t child=-1;
    ~Lease(){if(socket>=0)close(socket);if(child>0){while(waitpid(child,nullptr,0)<0&&errno==EINTR){}}}
};
inline std::shared_ptr<Lease> join(dev_t device){
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return {};
    auto address=vrProfile::address(getuid(),device);
    int result=connect(fd,reinterpret_cast<sockaddr*>(&address.value),address.size);
    if(result&&errno!=EINPROGRESS){close(fd);return {};}
    pollfd ready{fd,POLLIN,0};char status='E';ucred peer{};socklen_t length=sizeof(peer);
    if(poll(&ready,1,2000)<=0||getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length)||peer.uid!=getuid()||recv(fd,&status,1,0)!=1||status!='A'){
        close(fd);return {};
    }
    auto lease=std::make_shared<Lease>();lease->socket=fd;return lease;
}
static void libraryAnchor(){}
inline std::shared_ptr<void> acquire(unsigned renderMajor,unsigned renderMinor){
    if(const char*v=getenv("MUXLESS_VR_PROFILE"))if(!strcmp(v,"0"))return {};
    namespace fs=std::filesystem;
    static std::mutex mutex;
    static std::map<dev_t,std::weak_ptr<Lease>> leases;
    std::lock_guard<std::mutex> guard(mutex);
    dev_t device=makedev(renderMajor,renderMinor);
    if(auto previous=leases[device].lock()){
        pollfd status{previous->socket,POLLIN|POLLHUP,0};
        if(poll(&status,1,0)==0)return previous;
    }
    try{
        auto sys=fs::path("/sys/dev/char")/(std::to_string(renderMajor)+":"+std::to_string(renderMinor));
        auto node=fs::canonical(sys);auto name=node.filename().string();
        if(name.rfind("renderD",0)!=0)return {};
        std::ifstream file(sys/"device/pp_power_profile_mode");
        std::string table((std::istreambuf_iterator<char>(file)),{});
        if(parse(table).vr<0){std::fputs("Muxless VR profile: driver exposes no named VR mode; unchanged\n",stderr);return {};}
        if(auto previous=join(device)){leases[device]=previous;return previous;}
        Dl_info info{};
        if(!dladdr(reinterpret_cast<void*>(&libraryAnchor),&info)||!info.dli_fname)return {};
        auto helper=fs::path(info.dli_fname).parent_path()/"muxless-vr-profile";
        struct stat st{};
        if(stat(helper.c_str(),&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||!(st.st_mode&S_ISUID)||(st.st_mode&0022)){
            std::fputs("Muxless VR profile: VR mode is available but native helper needs installer authorization\n",stderr);return {};
        }
        int render=open((fs::path("/dev/dri")/name).c_str(),O_RDWR|O_CLOEXEC);
        if(render<0)return {};
        // Reserve the protocol fd before constructing the child socket actions.
        int renderCopy=fcntl(render,F_DUPFD_CLOEXEC,4);close(render);if(renderCopy<0)return {};
        int sockets[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets)){close(renderCopy);return {};}
        auto lease=std::make_shared<Lease>();lease->socket=sockets[0];
        posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions,sockets[1],0);
        posix_spawn_file_actions_addclose(&actions,sockets[0]);
        posix_spawn_file_actions_addclose(&actions,sockets[1]);
        posix_spawn_file_actions_adddup2(&actions,renderCopy,3);
        posix_spawn_file_actions_addclose(&actions,renderCopy);
        std::string executable=helper.string();char*args[]={const_cast<char*>(executable.c_str()),const_cast<char*>("--lease"),nullptr};
        int result=posix_spawn(&lease->child,executable.c_str(),&actions,nullptr,args,environ);
        posix_spawn_file_actions_destroy(&actions);close(sockets[1]);close(renderCopy);
        if(result){lease->child=-1;return {};}
        pollfd ready{lease->socket,POLLIN,0};char status='E';
        if(poll(&ready,1,2000)<=0||recv(lease->socket,&status,1,0)!=1||status!='A'){
            lease.reset();
            if(status=='B')for(int i=0;i<20;i++){
                if(auto existing=join(device)){leases[device]=existing;return existing;}
                usleep(10000);
            }
            std::fprintf(stderr,"Muxless VR profile: automatic selection unavailable (%c)\n",status);return {};
        }
        // The helper's short-lived parent has detached the unprivileged service.
        while(waitpid(lease->child,nullptr,0)<0&&errno==EINTR){}
        lease->child=-1;
        leases[device]=lease;return lease;
    }catch(const std::exception&e){std::fprintf(stderr,"Muxless VR profile: %s\n",e.what());return {};}
}
}
