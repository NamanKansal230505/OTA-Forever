#pragma once

#define OTAF_PORTAL_IP_STR "192.168.4.1"

/* Tiny captive-portal DNS server: answers every A query with the portal IP. */
void otaf_dns_start(void);
void otaf_dns_stop(void);
