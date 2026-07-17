#include "shutdown_signal.hpp"

volatile std::sig_atomic_t running = 1;

void sigIntHandler(__attribute__((unused)) int signum)
{
    running = 0;
}
