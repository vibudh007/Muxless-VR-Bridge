#include "../shim/gpu_launch.hpp"
#include <cassert>
#include <iostream>
static void process(const std::filesystem::path&root,const char*pid,const char*name,const std::string&environment,char state='S'){
 auto p=root/pid;std::filesystem::create_directories(p);std::ofstream(p/"comm")<<name<<"\n";std::ofstream(p/"stat")<<pid<<" (steam) "<<state<<" 0\n";std::ofstream(p/"environ",std::ios::binary).write(environment.data(),environment.size());
}
int main(){
 auto root=std::filesystem::temp_directory_path()/("wmr-gpu-launch-"+std::to_string(getpid()));
 std::string good="X=1";good+=char(0);good+="DRI_PRIME=1";good+=char(0);process(root,"1","steam",good);
 process(root,"2","steam","DRI_PRIME=0");
 process(root,"3","steam","DRI_PRIME=1",'Z');
 process(root,"4","steamwebhelper","DRI_PRIME=0");
 process(root,"5","steam","DRI_PRIME=10");
 auto ps=gpuLaunch::steamProcesses(root);assert(ps.size()==3);auto wrong=gpuLaunch::incompatible(ps);assert(wrong.find('2')!=std::string::npos&&wrong.find('5')!=std::string::npos&&wrong.find('1')==std::string::npos);
 std::filesystem::remove(root/"1/environ");ps=gpuLaunch::steamProcesses(root);assert(gpuLaunch::incompatible(ps).find('1')!=std::string::npos);
 std::filesystem::remove_all(root);std::cout<<"PASS: exact dGPU environment required; missing environment rejected; zombies and unrelated processes excluded.\n";
}
