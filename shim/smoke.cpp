#define main toy_main
#include "../prototype/main.cpp"
#undef main
int main(){std::cout.setf(std::ios::unitbuf);try{App app;app.discover();app.present.describe(app.render.physical);app.output();app.createDevices();app.resources("prototype/build/cube.spv");for(unsigned i=0;i<30;i++)app.frame(i,i/90.0f,i==0);std::cout<<"ALPHA_SMOKE_PASS\n";return 0;}catch(const std::exception&e){std::cerr<<"ALPHA_SMOKE_FAIL "<<e.what()<<"\n";return 1;}}
