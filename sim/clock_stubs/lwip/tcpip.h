#pragma once
#include "dns.h"
typedef void (*tcpip_callback_fn)(void* ctx);
err_t tcpip_callback(tcpip_callback_fn fn, void* ctx);   // runs fn on "lwIP's task" (sim/sim_net.cpp)
