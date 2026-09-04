#include "project/source/source_manager.hpp"
#include "metrics/source_acquisition_telemetry.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace cw::server;
int main(){
 std::ofstream("/mnt/data/server_entry_repair/testrepo/sm1.cpp")<<"struct A;\n";
 source_manager sm; assert(sm.initialize().ok()); auto u=sm.begin_update(); std::cerr<<"add\n"; assert(u.add("/mnt/data/server_entry_repair/testrepo/sm1.cpp",project_item_role::source).ok()); auto id=u.roots()[0].source; source_acquisition_telemetry t{metrics_mode::off}; std::cerr<<"acq\n"; auto r=u.acquire(id,t); std::cerr<<"acq done "<<(int)r.code<<" changes="<<u.changes().size()<<"\n"; assert(r.ok()); std::cerr<<"done\n";
}
