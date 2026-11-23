#pragma once

namespace otogi {

// With OTOGI_STACKS_EVERY=<seconds>, prints that often, for every thread, the
// function it is in and the exe addresses on its stack, named from the exe's
// symbols: lifted functions are sub_XXXXXXXX, so a hang shows which guest
// functions each thread is in. Off otherwise.
void start_stack_sampler();

}  // namespace otogi
