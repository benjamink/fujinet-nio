#ifndef FUJINET_PICO2W_LWIPOPTS_H
#define FUJINET_PICO2W_LWIPOPTS_H

// lwIP in NO_SYS mode with cyw43_arch_lwip_threadsafe_background: raw API only
// (no BSD sockets). Sizes follow pico-examples' pico_w lwipopts and are the
// first thing to tune once real TCP/UDP traffic runs (README, "Memory").
#include "../src/platform/pico/detail/network_limits.h"

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (128 * 1024) // ALTCP installs this bounded heap as the mbedTLS allocator
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define TCP_LISTEN_BACKLOG          1
#define SO_REUSE                   1
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_MSS                     1460
#define TCP_WND                     FN_PICO_TCP_WINDOW_BYTES // full encrypted TLS record; RawTcp ring matches
#define TCP_SND_BUF                 (2 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETCONN                0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
#define ALTCP_MBEDTLS_AUTHMODE      MBEDTLS_SSL_VERIFY_REQUIRED
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETBUF_RECVINFO        0
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#endif
