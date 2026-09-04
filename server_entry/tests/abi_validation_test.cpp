#include "project/graph/graph.hpp"
#include "project/graph/compiled_state.hpp"
#include <cassert>
#include <iostream>
using namespace cw::server;
int main(){
  graph g;
  abi_configuration bad{}; bad.target=abi_target::posix_x64; bad.pack=3;
  assert(!g.initialize(bad).ok());
  abi_configuration good{}; good.target=abi_target::posix_x64; good.pack=8;
  assert(g.initialize(good).ok());
  compiled_graph_state s; s.abi=bad;
  assert(!g.import_compiled(s).ok());
  std::cout<<"PASS\n";
}
