#include <clients/rt/PitchClient.hpp>
#include <FluidMaxWrapper.hpp>

void ext_main(void*)
{
  using namespace fluid::client;
  makeMaxWrapper<PitchClient>("fluid.pitch~");
}