// SPDX-License-Identifier: GPL-3.0-or-later
// The privileged surface accepts only a render-device fd and a same-user lease
// socket. It opens one kernel-owned profile control, then permanently drops root.
#include "vr_profile.hpp"
#include "profile_protocol.hpp"
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/file.h>
#include <linux/magic.h>
#include <poll.h>
#include <grp.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <stdexcept>
namespace fs=std::filesystem;
static volatile sig_atomic_t stopping=0;
static void stop(int){stopping=1;}
struct Fd {
    int value=-1;
    explicit Fd(int n=-1):value(n){}
    ~Fd(){if(value>=0)close(value);}
};
static std::string readControl(int fd){
    if(lseek(fd,0,SEEK_SET)<0)throw std::runtime_error("Cannot rewind profile control");
    char buffer[32768];ssize_t n;
    do{n=read(fd,buffer,sizeof(buffer));}while(n<0&&errno==EINTR);
    if(n<0||n==ssize_t(sizeof(buffer)))throw std::runtime_error("Cannot read profile table");
    return std::string(buffer,static_cast<size_t>(n));
}
static void select(int fd,int id){
    auto text=std::to_string(id)+"\n";
    if(lseek(fd,0,SEEK_SET)<0||write(fd,text.data(),text.size())!=ssize_t(text.size()))
        throw std::runtime_error("Driver rejected VR profile request");
    if(vrProfile::parse(readControl(fd)).current!=id)
        throw std::runtime_error("Driver did not retain requested profile");
}
static void drop(uid_t uid,gid_t gid){
    if(geteuid()==0&&setgroups(0,nullptr))throw std::runtime_error("Cannot drop groups");
    if(setresgid(gid,gid,gid)||setresuid(uid,uid,uid))throw std::runtime_error("Cannot drop privileges");
}
static void reply(char result){(void)send(STDIN_FILENO,&result,1,MSG_NOSIGNAL);}
static int authorize(){
    if(getuid()!=0||geteuid()!=0)throw std::runtime_error("Authorization requires an administrator");
    Fd self(open("/proc/self/exe",O_RDONLY|O_CLOEXEC));struct stat st{};
    if(self.value<0||fstat(self.value,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1)
        throw std::runtime_error("Invalid helper installation");
    // Operate on the executable inode, never a path supplied by the caller.
    if(fchown(self.value,0,0)||fchmod(self.value,04755))
        throw std::runtime_error("Cannot authorize native VR profile helper");
    std::puts("Native VR profile authorization installed.");return 0;
}
int main(int argc,char**argv){
    int restore=-1,vr=-1;Fd control;bool owns=false;
    try{
        signal(SIGPIPE,SIG_IGN);
        if(argc==2&&!strcmp(argv[1],"--authorize"))return authorize();
        if(argc!=2||strcmp(argv[1],"--lease"))throw std::runtime_error("Expected --lease");
        uid_t uid=getuid();gid_t gid=getgid();
        ucred peer{};socklen_t size=sizeof(peer);int type=0;socklen_t typeSize=sizeof(type);
        if(getsockopt(0,SOL_SOCKET,SO_TYPE,&type,&typeSize)||type!=SOCK_SEQPACKET||
           getsockopt(0,SOL_SOCKET,SO_PEERCRED,&peer,&size)||peer.uid!=uid)
            throw std::runtime_error("Expected a same-user lease socket");
        struct stat render{};
        if(fstat(3,&render)||!S_ISCHR(render.st_mode))throw std::runtime_error("Expected render-device fd 3");
        auto sys=fs::path("/sys/dev/char")/(std::to_string(major(render.st_rdev))+":"+std::to_string(minor(render.st_rdev)));
        auto node=fs::canonical(sys);
        if(node.filename().string().rfind("renderD",0)!=0||fs::canonical(sys/"subsystem")!="/sys/class/drm")
            throw std::runtime_error("Fd 3 is not a DRM render node");
        auto device=fs::canonical(sys/"device");
        auto path=device/"pp_power_profile_mode";
        // Absence is a normal unsupported capability, never a reason to force clocks.
        control.value=open(path.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(control.value<0){int saved=errno;drop(uid,gid);reply(saved==ENOENT?'N':saved==EACCES?'P':'E');return saved==ENOENT?0:1;}
        struct stat st{};struct statfs filesystem{};
        if(fstat(control.value,&st)||st.st_uid!=0||!S_ISREG(st.st_mode)||
           fstatfs(control.value,&filesystem)||filesystem.f_type!=SYSFS_MAGIC)
            throw std::runtime_error("Profile control is not kernel-owned sysfs");
        if(flock(control.value,LOCK_EX|LOCK_NB)){
            drop(uid,gid);reply('B');return 0;
        }
        const auto deviceId=render.st_rdev;
        close(3);drop(uid,gid);
        const auto profiles=vrProfile::parse(readControl(control.value));
        if(profiles.vr<0||profiles.currentName=="CUSTOM"){reply('N');return 0;}
        if(profiles.current<0)throw std::runtime_error("Driver did not identify its active profile");
        restore=profiles.current;vr=profiles.vr;
        // A detached, unprivileged lease service permits multiple devices/processes
        // to share this GPU policy. Closing the last socket restores the profile.
        const auto service=fork();
        if(service<0)throw std::runtime_error("Cannot start VR profile lease service");
        if(service>0)return 0;
        if(setsid()<0)throw std::runtime_error("Cannot detach VR profile service");
        Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
        auto address=vrProfile::address(uid,deviceId);
        if(listener.value<0||bind(listener.value,reinterpret_cast<sockaddr*>(&address.value),address.size)||listen(listener.value,16))
            throw std::runtime_error("Cannot establish GPU profile lease endpoint");
        signal(SIGTERM,stop);signal(SIGINT,stop);
        owns=true;select(control.value,vr);
        std::vector<int> clients{STDIN_FILENO};
        if(send(0,"A",1,MSG_NOSIGNAL)!=1)stopping=1;
        std::fprintf(stderr,"Muxless VR profile: selected driver VR profile %d; previous=%d\n",vr,restore);
        while(!stopping&&!clients.empty()){
            std::vector<pollfd> sockets{{listener.value,POLLIN,0}};
            for(int fd:clients)sockets.push_back({fd,POLLIN|POLLHUP,0});
            int result=poll(sockets.data(),sockets.size(),1000);
            if(result<0){if(errno==EINTR)continue;break;}
            for(size_t i=clients.size();i>0;--i)if(sockets[i].revents){
                close(clients[i-1]);clients.erase(clients.begin()+i-1);
            }
            if(sockets[0].revents&POLLIN){
                int fd;
                while((fd=accept4(listener.value,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK))>=0){
                    ucred incoming{};socklen_t length=sizeof(incoming);
                    if(clients.size()<128&&!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&incoming,&length)&&incoming.uid==uid&&send(fd,"A",1,MSG_NOSIGNAL)==1)
                        clients.push_back(fd);
                    else close(fd);
                }
            }
            const auto observed=vrProfile::parse(readControl(control.value));
            const auto active=observed.current;
            if(observed.currentName=="CUSTOM"){owns=false;std::fputs("Muxless VR profile: preserving external custom profile\n",stderr);return 0;}
            if(active<0)throw std::runtime_error("Driver profile state became unavailable");
            if(active!=vr&&!clients.empty()){
                restore=active;select(control.value,vr);
                std::fprintf(stderr,"Muxless VR profile: renewed VR after external profile change\n");
            }
        }
        if(vrProfile::parse(readControl(control.value)).current==vr)select(control.value,restore);
        owns=false;
        std::fprintf(stderr,"Muxless VR profile: restored profile %d\n",restore);return 0;
    }catch(const std::exception&e){
        if(owns&&control.value>=0){try{if(vrProfile::parse(readControl(control.value)).current==vr)select(control.value,restore);}catch(...){std::fprintf(stderr,"Muxless VR profile: restoration failed\n");}}
        reply('E');std::fprintf(stderr,"Muxless VR profile: %s\n",e.what());return 1;
    }
}
