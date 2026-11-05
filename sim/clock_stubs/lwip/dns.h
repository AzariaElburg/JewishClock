#pragma once
// PC stand-in (jcclock): lwIP's name lookup, done by the PC (sim/sim_net.cpp)
// and answered through the callback from another thread, as lwIP's own task
// answers it on the clock.
#include <cstdint>
typedef int8_t err_t;
#define ERR_OK          0
#define ERR_INPROGRESS  (-5)
#define ERR_ARG         (-16)
#define ERR_MEM         (-1)
#define LWIP_IPV4 1
#define LWIP_IPV6 1
#define LWIP_DNS_ADDRTYPE_IPV4 0
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip4; uint8_t type; } ip_addr_t;   // type 0 = IPv4
#define IP_IS_V4(ipaddr) ((ipaddr)->type == 0)
#define ip_2_ip4(ipaddr) (&((ipaddr)->ip4))
#define ip4_addr_get_u32(src_ipaddr) ((src_ipaddr)->addr)
typedef void (*dns_found_callback)(const char* name, const ip_addr_t* ipaddr, void* arg);
err_t dns_gethostbyname_addrtype(const char* hostname, ip_addr_t* addr, dns_found_callback found, void* arg, uint8_t type);
err_t dns_gethostbyname(const char* hostname, ip_addr_t* addr, dns_found_callback found, void* arg);
