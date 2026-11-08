#pragma once
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

// Management probes only. Drain even discarded output so a verbose child cannot
// deadlock on a full pipe. A process group also covers inherited pipes in children.
inline std::pair<int,std::string> boundedCommand(
    const std::vector<std::string>& args,
    std::chrono::milliseconds timeout=std::chrono::seconds(15)) {
    if(args.empty()||timeout.count()<0)throw std::runtime_error("Invalid probe command");
    std::vector<char*> argv;for(const auto&arg:args)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);
    int pipefd[2];if(pipe2(pipefd,O_CLOEXEC))throw std::runtime_error("Probe pipe failed");
    if(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)<0){close(pipefd[0]);close(pipefd[1]);throw std::runtime_error("Probe pipe setup failed");}
    const pid_t pid=fork();
    if(pid==0){
        setpgid(0,0);dup2(pipefd[1],STDOUT_FILENO);dup2(pipefd[1],STDERR_FILENO);
        close(pipefd[0]);close(pipefd[1]);execvp(argv[0],argv.data());_exit(127);
    }
    close(pipefd[1]);if(pid<0){close(pipefd[0]);throw std::runtime_error("Probe fork failed");}
    setpgid(pid,pid);
    struct Child {
        pid_t pid;int fd;bool reaped=false,finished=false;
        void terminate(){kill(-pid,SIGKILL);if(!reaped)kill(pid,SIGKILL);}
        ~Child(){if(!finished)terminate();if(!reaped){int status;waitpid(pid,&status,WNOHANG);}close(fd);}
    } child{pid,pipefd[0]};
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    bool eof=false,timedOut=false;int status=0;std::string output;char buffer[4096];
    constexpr size_t limit=1024*1024;
    while(!eof||!child.reaped){
        // Bound a drain pass, too: continuous output must not starve the deadline.
        for(unsigned reads=0;reads<64&&!eof;reads++){
            auto n=read(child.fd,buffer,sizeof(buffer));
            if(n>0){const auto keep=std::min(size_t(n),limit-output.size());output.append(buffer,keep);continue;}
            if(n==0)eof=true;
            else if(errno==EINTR)continue;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK)throw std::runtime_error("Probe read failed");
            break;
        }
        if(!child.reaped){pid_t result;do{result=waitpid(pid,&status,WNOHANG);}while(result<0&&errno==EINTR);
            if(result==pid)child.reaped=true;else if(result<0)throw std::runtime_error("Probe wait failed");}
        if(eof&&child.reaped)break;
        const auto now=std::chrono::steady_clock::now();
        if(now>=deadline){timedOut=true;child.terminate();break;}
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
        pollfd descriptor{child.fd,POLLIN,0};int result;
        do{result=poll(eof?nullptr:&descriptor,eof?0:1,int(std::min<int64_t>(left+1,25)));}while(result<0&&errno==EINTR&&std::chrono::steady_clock::now()<deadline);
        if(result<0&&errno!=EINTR)throw std::runtime_error("Probe poll failed");
    }
    if(timedOut){
        // Reap ordinary killed probes, but don't wait forever on a kernel-stuck one.
        for(unsigned i=0;i<25&&!child.reaped;i++){
            auto result=waitpid(pid,&status,WNOHANG);if(result==pid|| (result<0&&errno==ECHILD)){child.reaped=true;break;}
            poll(nullptr,0,2);
        }
        output+="\nProbe timed out; process group terminated.\n";child.finished=true;return {124,output};
    }
    child.finished=true;
    return {WIFEXITED(status)?WEXITSTATUS(status):(WIFSIGNALED(status)?128+WTERMSIG(status):128),output};
}
