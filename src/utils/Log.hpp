// Log leve: só emite quando compilado com `make DEBUG=1` (saída via nxlink).
#pragma once

#include <cstdio>

#ifdef SWITCHCAST_DEBUG
#define SC_LOG(fmt, ...) std::printf("[SwitchCast] " fmt "\n", ##__VA_ARGS__)
#else
#define SC_LOG(fmt, ...) ((void)0)
#endif
