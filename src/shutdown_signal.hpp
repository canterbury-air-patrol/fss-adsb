#pragma once

#include <csignal>

/* Process-lifetime flag toggled by sigIntHandler on SIGINT/SIGTERM and
 * polled by main()'s loop. sig_atomic_t so the write from a signal handler
 * is safe without further synchronisation -- the only guarantee C++ makes
 * about state touched inside a signal handler. Extracted from main.cpp (which
 * is not linked into the test binary, since it defines its own main()) so
 * the handler itself is directly testable: register it and raise a signal,
 * or call it directly, and assert running flips to 0. */
extern volatile std::sig_atomic_t running;

void sigIntHandler(int signum);
