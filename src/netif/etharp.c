/**
 * @file
 * Address Resolution Protocol module for IP over Ethernet
 *
 * Functionally, ARP is divided into two parts. The first maps an IP address
 * to a physical address when sending a packet, and the second part answers
 * requests from other machines for our physical address.
 *
 * This implementation complies with RFC 826 (Ethernet ARP). It supports
 * Gratuitious ARP from RFC3220 (IP Mobility Support for IPv4) section 4.6
 * if an interface calls etharp_query(our_netif, its_ip_addr, NULL) upon
 * address change.
 */

/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * Copyright (c) 2003-2004 Leon Woestenberg <leon.woestenberg@axon.tv>
 * Copyright (c) 2003-2004 Axon Digital Design B.V., The Netherlands.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 */

#include "lwip/opt.h"
#include "lwip/inet.h"
#include "netif/etharp.h"
#include "lwip/ip.h"
#include "lwip/stats.h"

/* ARP needs to inform DHCP of any ARP replies? */
#if (LWIP_DHCP && DHCP_DOES_ARP_CHECK)
#  include "lwip/dhcp.h"
#endif

/* allows new queueing code to be disabled (0) for regression testing */
#define ARP_NEW_QUEUE 1

/** the time an ARP entry stays valid after its last update, (120 * 10) seconds = 20 minutes. */
#define ARP_MAXAGE 120
/** the time an ARP entry stays pending after first request, (1 * 10) seconds = 10 seconds. */
#define ARP_MAXPENDING 1

#define HWTYPE_ETHERNET 1

/** ARP message types */
#define ARP_REQUEST 1
#define ARP_REPLY 2

#define ARPH_HWLEN(hdr) (ntohs((hdr)->_hwlen_protolen) >> 8)
#define ARPH_PROTOLEN(hdr) (ntohs((hdr)->_hwlen_protolen) & 0xff)

#define ARPH_HWLEN_SET(hdr, len) (hdr)->_hwlen_protolen = htons(ARPH_PROTOLEN(hdr) | ((len) << 8))
#define ARPH_PROTOLEN_SET(hdr, len) (hdr)->_hwlen_protolen = htons((len) | (ARPH_HWLEN(hdr) << 8))

enum etharp_state {
  ETHARP_STATE_EMPTY,
  ETHARP_STATE_PENDING,
  ETHARP_STATE_STABLE,
  /** @internal convenience transitional state used in etharp_tmr() */
  ETHARP_STATE_EXPIRED
};

struct etharp_entry {
  struct ip_addr ipaddr;
  struct eth_addr ethaddr;
  enum etharp_state state;
#if ARP_QUEUEING
  /** 
   * Pointer to queue of pending outgoing packets on this ARP entry.
   * Must be at most a single packet for now. */
  struct pbuf *p;
#endif
  u8_t ctime;
};

static const struct eth_addr ethbroadcast = {{0xff,0xff,0xff,0xff,0xff,0xff}};
static struct etharp_entry arp_table[ARP_TABLE_SIZE];

static s8_t find_arp_entry(void);
/** ask update_arp_entry() to add instead of merely update an ARP entry */
#define ARP_INSERT_FLAG 1
static struct pbuf *update_arp_entry(struct netif *netif, struct ip_addr *ipaddr, struct eth_addr *ethaddr, u8_t flags);
/**
 * Initializes ARP module.
 */
void
etharp_init(void)
{
  s8_t i;
  /* clear ARP entries */
  for(i = 0; i < ARP_TABLE_SIZE; ++i) {
    arp_table[i].state = ETHARP_STATE_EMPTY;
#if ARP_QUEUEING
    arp_table[i].p = NULL;
#endif
    arp_table[i].ctime = 0;
  }
}

/**
 * Clears expired entries in the ARP table.
 *
 * This function should be called every ETHARP_TMR_INTERVAL microseconds (10 seconds),
 * in order to expire entries in the ARP table.
 */
void
etharp_tmr(void)
{
  s8_t i;

  LWIP_DEBUGF(ETHARP_DEBUG, ("etharp_timer\n"));
  /* remove expired entries from the ARP table */
  for (i = 0; i < ARP_TABLE_SIZE; ++i) {
    arp_table[i].ctime++;
    /* a resolved/stable entry? */
    if ((arp_table[i].state == ETHARP_STATE_STABLE) &&
         /* entry has become old? */
        (arp_table[i].ctime >= ARP_MAXAGE)) {
      LWIP_DEBUGF(ETHARP_DEBUG, ("etharp_timer: expired stable entry %u.\n", i));
      arp_table[i].state = ETHARP_STATE_EXPIRED;
    /* an unresolved/pending entry? */
    } else if ((arp_table[i].state == ETHARP_STATE_PENDING) &&
         /* entry unresolved/pending for too long? */
        (arp_table[i].ctime >= ARP_MAXPENDING)) {
      LWIP_DEBUGF(ETHARP_DEBUG, ("etharp_timer: expired pending entry %u.\n", i));
      arp_table[i].state = ETHARP_STATE_EXPIRED;
    }
    /* clean up entries that have just been expired */
    if (arp_table[i].state == ETHARP_STATE_EXPIRED) {
#if ARP_QUEUEING
      /* and empty packet queue */
      if (arp_table[i].p != NULL) {
        /* remove all queued packets */
        LWIP_DEBUGF(ETHARP_DEBUG, ("etharp_timer: freeing entry %u, packet queue %p.\n", i, (void *)(arp_table[i].p)));
        pbuf_free(arp_table[i].p);
        arp_table[i].p = NULL;
      }
#endif
      /* recycle entry for re-use */      
      arp_table[i].state = ETHARP_STATE_EMPTY;
    }
  }
}

/**
 * Return an empty ARP entry (possibly recycling the oldest stable entry).
 *
 * @return The ARP entry index that is available, ERR_MEM if no usable
 * entry is found.
 */
static s8_t
find_arp_entry(void)
{
  s8_t i, j;
  u8_t maxtime = 0;

  j = ARP_TABLE_SIZE;
  /* search ARP table for an unused or old entry */
  for (i = 0; i < ARP_TABLE_SIZE; ++i) {
  	/* empty entry? */
    if (arp_table[i].state == ETHARP_STATE_EMPTY) {
      LWIP_DEBUGF(ETHARP_DEBUG, ("find_arp_entry: returning empty entry %u\n", i));
      return i;
  	/* stable entry? */
    } else if (arp_table[i].state == ETHARP_STATE_STABLE) {
      /* remember entry with oldest stable entry in j */
      if (arp_table[i].ctime >= maxtime) maxtime = arp_table[j = i].ctime;
    }
  }
  /* no empty entry found? */
  if (i == ARP_TABLE_SIZE) {
  	LWIP_DEBUGF(ETHARP_DEBUG, ("find_arp_entry: found oldest stable entry %u\n", j));
    /* fall-back to oldest stable */
  	i = j;
  }
  /* no available entry found? */
  if (i == ARP_TABLE_SIZE) {
    LWIP_DEBUGF(ETHARP_DEBUG, ("find_arp_entry: no replacable entry could be found\n"));
    /* return failure */
    return ERR_MEM;
  }

  /* clean up the oldest stable entry (to be recycled) */
  if (arp_table[i].state == ETHARP_STATE_STABLE) {
#if ARP_QUEUEING
    /* and empty the packet queue */
    if (arp_table[i].p != NULL) {
      LWIP_DEBUGF(ETHARP_DEBUG, ("find_arp_entry: freeing entry %u, packet queue %p.\n", i, (void *)(arp_table[i].p)));
      /* remove all queued packets */
      pbuf_free(arp_table[i].p);
      arp_table[i].p = NULL;
    }
#endif
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("find_arp_entry: recycling oldest stable entry %u\n", i));
    arp_table[i].state = ETHARP_STATE_EMPTY;
  }
  LWIP_DEBUGF(ETHARP_DEBUG, ("find_arp_entry: returning %u\n", i));
  return i;
}

/**
 * Update (or insert) a IP/MAC address pair in the ARP cache.
 *
 * If a pending entry is resolved, any queued packets will be sent
 * at this point.
 * 
 * @param ipaddr IP address of the inserted ARP entry.
 * @param ethaddr Ethernet address of the inserted ARP entry.
 * @param flags Defines behaviour:
 * - ARP_INSERT_FLAG Allows ARP to insert this as a new item. If not specified,
 * only existing ARP entries will be updated.
 *
 * @return pbuf If non-NULL, a packet that was queued on a pending entry.
 * You should sent it and must call pbuf_free() afterwards.
 *
 * @see pbuf_free()
 */
static struct pbuf *
update_arp_entry(struct netif *netif, struct ip_addr *ipaddr, struct eth_addr *ethaddr, u8_t flags)
{
  s8_t i, k;
  LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE | 3, ("update_arp_entry()\n"));
  LWIP_ASSERT("netif->hwaddr_len != 0", netif->hwaddr_len != 0);
  LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: %u.%u.%u.%u - %02x:%02x:%02x:%02x:%02x:%02x\n",
                                        ip4_addr1(ipaddr), ip4_addr2(ipaddr), ip4_addr3(ipaddr), ip4_addr4(ipaddr), 
                                        ethaddr->addr[0], ethaddr->addr[1], ethaddr->addr[2],
                                        ethaddr->addr[3], ethaddr->addr[4], ethaddr->addr[5]));
  /* do not update for 0.0.0.0 addresses */
  if (ipaddr->addr == 0) {
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: will not add 0.0.0.0 to ARP cache\n"));
    return NULL;
  }
  /* Walk through the ARP mapping table and try to find an entry to update.
   * If none is found, a new IP -> MAC address mapping is inserted. */
  for (i = 0; i < ARP_TABLE_SIZE; ++i) {
    /* Check if the source IP address of the incoming packet matches
     * the IP address in this ARP table entry. */
    if (arp_table[i].state != ETHARP_STATE_EMPTY &&
    	ip_addr_cmp(ipaddr, &arp_table[i].ipaddr)) {
      /* pending entry? */
      if (arp_table[i].state == ETHARP_STATE_PENDING) {
        LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: pending entry %u goes stable\n", i));
        /* A pending entry was found, mark it stable */
        arp_table[i].state = ETHARP_STATE_STABLE;
        /* fall-through to next if */
      }
      /* stable entry? (possibly just marked stable) */
      if (arp_table[i].state == ETHARP_STATE_STABLE) {
        LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: updating stable entry %u\n", i));
        /* An old entry found, update this and return. */
        for (k = 0; k < netif->hwaddr_len; ++k) {
          arp_table[i].ethaddr.addr[k] = ethaddr->addr[k];
        }
        /* reset time stamp */
        arp_table[i].ctime = 0;
/* this is where we will send out queued packets! */
#if ARP_QUEUEING
        while (arp_table[i].p != NULL) {
          /* get the first packet on the queue (if any) */
          struct pbuf *p = arp_table[i].p;
          /* Ethernet header */
          struct eth_hdr *ethhdr = p->payload;;
          /* remember (and reference) remainder of queue */
          /* note: this will also terminate the p pbuf chain */
          arp_table[i].p = pbuf_dequeue(p);
          /* fill-in Ethernet header */
          for (k = 0; k < netif->hwaddr_len; ++k) {
            ethhdr->dest.addr[k] = ethaddr->addr[k];
            ethhdr->src.addr[k] = netif->hwaddr[k];
          }
          ethhdr->type = htons(ETHTYPE_IP);
          LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: sending queued IP packet %p.\n", (void *)p));
          /* send the queued IP packet */
          netif->linkoutput(netif, p);
          /* free the queued IP packet */
          pbuf_free(p);
        }
#endif
        /* IP addresses should only occur once in the ARP entry, we are done */
        return NULL;
      }
    } /* if STABLE */
  } /* for all ARP entries */

  /* no matching ARP entry was found */
  LWIP_ASSERT("update_arp_entry: i == ARP_TABLE_SIZE", i == ARP_TABLE_SIZE);

  LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: IP address not yet in table\n"));
  /* allowed to insert a new entry? */
  if (flags & ARP_INSERT_FLAG)
  {
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: adding entry to table\n"));
    /* find an empty or old entry. */
    i = find_arp_entry();
    if (i == ERR_MEM) {
      LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: no available entry found\n"));
      return NULL;
    }
    /* set IP address */
    ip_addr_set(&arp_table[i].ipaddr, ipaddr);
    /* set Ethernet hardware address */
    for (k = 0; k < netif->hwaddr_len; ++k) {
      arp_table[i].ethaddr.addr[k] = ethaddr->addr[k];
    }
    /* reset time-stamp */
    arp_table[i].ctime = 0;
    /* mark as stable */
    arp_table[i].state = ETHARP_STATE_STABLE;
    /* no queued packet */
#if ARP_QUEUEING
    arp_table[i].p = NULL;
#endif
  }
  else
  {
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("update_arp_entry: no matching stable entry to update\n"));
  }
  return NULL;
}

/**
 * Updates the ARP table using the given IP packet.
 *
 * Uses the incoming IP packet's source address to update the
 * ARP cache for the local network. The function does not alter
 * or free the packet. This function must be called before the
 * packet p is passed to the IP layer.
 *
 * @param netif The lwIP network interface on which the IP packet pbuf arrived.
 * @param pbuf The IP packet that arrived on netif.
 *
 * @return NULL
 *
 * @see pbuf_free()
 */
void
etharp_ip_input(struct netif *netif, struct pbuf *p)
{
  struct ethip_hdr *hdr;

  /* Only insert an entry if the source IP address of the
     incoming IP packet comes from a host on the local network. */
  hdr = p->payload;
  /* source is on local network? */
  if (!ip_addr_maskcmp(&(hdr->ip.src), &(netif->ip_addr), &(netif->netmask))) {
    /* do nothing */
    return;
  }

  LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_ip_input: updating ETHARP table.\n"));
  /* update ARP table, ask to insert entry */
  update_arp_entry(netif, &(hdr->ip.src), &(hdr->eth.src), ARP_INSERT_FLAG);
}


/**
 * Responds to ARP requests to us. Upon ARP replies to us, add entry to cache  
 * send out queued IP packets. Updates cache with snooped address pairs.
 *
 * Should be called for incoming ARP packets. The pbuf in the argument
 * is freed by this function.
 *
 * @param netif The lwIP network interface on which the ARP packet pbuf arrived.
 * @param pbuf The ARP packet that arrived on netif. Is freed by this function.
 * @param ethaddr Ethernet address of netif.
 *
 * @return NULL
 *
 * @see pbuf_free()
 */
void
etharp_arp_input(struct netif *netif, struct eth_addr *ethaddr, struct pbuf *p)
{
  struct etharp_hdr *hdr;
  /* these are aligned properly, whereas the ARP header fields might not be */
  struct ip_addr sipaddr, dipaddr;
  u8_t i;
  u8_t for_us;

  /* drop short ARP packets */
  if (p->tot_len < sizeof(struct etharp_hdr)) {
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE | 1, ("etharp_arp_input: packet dropped, too short (%d/%d)\n", p->tot_len, sizeof(struct etharp_hdr)));
    pbuf_free(p);
    return;
  }

  hdr = p->payload;
 
  /* get aligned copies of addresses */
  *(struct ip_addr2 *)&sipaddr = hdr->sipaddr;
  *(struct ip_addr2 *)&dipaddr = hdr->dipaddr;

  /* this interface is not configured? */
  if (netif->ip_addr.addr == 0) {
    for_us = 0;
  } else {
    /* ARP packet directed to us? */
    for_us = ip_addr_cmp(&dipaddr, &(netif->ip_addr));
  }

  /* ARP message directed to us? */
  if (for_us) {
    /* add IP address in ARP cache; assume requester wants to talk to us.
     * can result in directly sending the queued packets for this host. */
    update_arp_entry(netif, &sipaddr, &(hdr->shwaddr), ARP_INSERT_FLAG);
  /* ARP message not directed to us? */
  } else {
    /* update the source IP address in the cache, if present */
    update_arp_entry(netif, &sipaddr, &(hdr->shwaddr), 0);
  }

  /* now act on the message itself */
  switch (htons(hdr->opcode)) {
  /* ARP request? */
  case ARP_REQUEST:
    /* ARP request. If it asked for our address, we send out a
     * reply. In any case, we time-stamp any existing ARP entry,
     * and possiby send out an IP packet that was queued on it. */

    LWIP_DEBUGF (ETHARP_DEBUG | DBG_TRACE, ("etharp_arp_input: incoming ARP request\n"));
    /* ARP request for our address? */
    if (for_us) {

      LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_arp_input: replying to ARP request for our IP address\n"));
      /* re-use pbuf to send ARP reply */
      hdr->opcode = htons(ARP_REPLY);

      hdr->dipaddr = hdr->sipaddr;
      hdr->sipaddr = *(struct ip_addr2 *)&netif->ip_addr;

      for(i = 0; i < netif->hwaddr_len; ++i) {
        hdr->dhwaddr.addr[i] = hdr->shwaddr.addr[i];
        hdr->shwaddr.addr[i] = ethaddr->addr[i];
        hdr->ethhdr.dest.addr[i] = hdr->dhwaddr.addr[i];
        hdr->ethhdr.src.addr[i] = ethaddr->addr[i];
      }

      hdr->hwtype = htons(HWTYPE_ETHERNET);
      ARPH_HWLEN_SET(hdr, netif->hwaddr_len);

      hdr->proto = htons(ETHTYPE_IP);
      ARPH_PROTOLEN_SET(hdr, sizeof(struct ip_addr));

      hdr->ethhdr.type = htons(ETHTYPE_ARP);
      /* return ARP reply */
      netif->linkoutput(netif, p);
    /* we are not configured? */
    } else if (netif->ip_addr.addr == 0) {
      /* { for_us == 0 and netif->ip_addr.addr == 0 } */
      LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_arp_input: we are unconfigured, ARP request ignored.\n"));
    /* request was not directed to us */
    } else {
      /* { for_us == 0 and netif->ip_addr.addr != 0 } */
      LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_arp_input: ARP request was not for us.\n"));
    }
    break;
  case ARP_REPLY:
    /* ARP reply. We already updated the ARP cache earlier. */
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_arp_input: incoming ARP reply\n"));
#if (LWIP_DHCP && DHCP_DOES_ARP_CHECK)
    /* DHCP wants to know about ARP replies to our wanna-have-address */
    if (for_us) dhcp_arp_reply(netif, &sipaddr);
#endif
    break;
  default:
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_arp_input: ARP unknown opcode type %d\n", htons(hdr->opcode)));
    break;
  }
  /* free ARP packet */
  pbuf_free(p);
}

/**
 * Resolve and fill-in Ethernet address header for outgoing packet.
 *
 * If ARP has the Ethernet address in cache, the given packet is
 * returned, ready to be sent.
 *
 * If ARP does not have the Ethernet address in cache the packet is
 * queued (if enabled and space available) and a ARP request is sent.
 * This ARP request is returned as a pbuf, which should be sent by
 * the caller.
 *
 * A returned non-NULL packet should be sent by the caller.
 *
 * If ARP failed to allocate resources, NULL is returned.
 *
 * @param netif The lwIP network interface which the IP packet will be sent on.
 * @param ipaddr The IP address of the packet destination.
 * @param pbuf The pbuf(s) containing the IP packet to be sent.
 *
 * @return If non-NULL, a packet ready to be sent by caller.
 *
 * @return
 * - ERR_BUF Could not make room for Ethernet header.
 * - ERR_MEM Hardware address unknown, and no more ARP entries available
 *   to query for address or queue the packet.
 * - ERR_RTE No route to destination (no gateway to external networks).
 */
err_t
etharp_output(struct netif *netif, struct ip_addr *ipaddr, struct pbuf *q)
{
  struct eth_addr *dest, *srcaddr, mcastaddr;
  struct eth_hdr *ethhdr;
  s8_t i;
  err_t result;

  /* make room for Ethernet header - should not fail*/
  if (pbuf_header(q, sizeof(struct eth_hdr)) != 0) {
    /* bail out */
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE | 2, ("etharp_output: could not allocate room for header.\n"));
    LINK_STATS_INC(link.lenerr);
    pbuf_free(q);
    return ERR_BUF;
  }

  /* assume unresolved Ethernet address */
  dest = NULL;
  /* Determine on destination hardware address. Broadcasts and multicasts
   * are special, other IP addresses are looked up in the ARP table. */

  /* destination IP address is an IP broadcast address? */
  if (ip_addr_isany(ipaddr) || ip_addr_isbroadcast(ipaddr, netif)) {
    /* broadcast on Ethernet also */
    dest = (struct eth_addr *)&ethbroadcast;
  }
  /* destination IP address is an IP multicast address? */
  else if (ip_addr_ismulticast(ipaddr)) {
    /* Hash IP multicast address to MAC address. */
    mcastaddr.addr[0] = 0x01;
    mcastaddr.addr[1] = 0x00;
    mcastaddr.addr[2] = 0x5e;
    mcastaddr.addr[3] = ip4_addr2(ipaddr) & 0x7f;
    mcastaddr.addr[4] = ip4_addr3(ipaddr);
    mcastaddr.addr[5] = ip4_addr4(ipaddr);
    /* destination Ethernet address is multicast */
    dest = &mcastaddr;
  }
  /* destination IP address is an IP unicast address */
  else {
    /* outside local network? */
    if (!ip_addr_maskcmp(ipaddr, &(netif->ip_addr), &(netif->netmask))) {
      /* interface has default gateway? */
      if (netif->gw.addr != 0) {
        /* send to hardware address of default gateway IP address */
        ipaddr = &(netif->gw);
      /* no default gateway available? */
      } else {
        /* destination unreachable, discard packet */
        pbuf_free(q);
        return ERR_RTE;
      }
    }
    result = etharp_query(netif, ipaddr, q);
  } /* else unicast */

  /* destination Ethernet address known */
  if (dest != NULL) {
    /* obtain source Ethernet address of the given interface */
    srcaddr = (struct eth_addr *)netif->hwaddr;
    /* A valid IP->MAC address mapping was found, fill in the
     * Ethernet header for the outgoing packet */
    ethhdr = q->payload;
    for(i = 0; i < netif->hwaddr_len; i++) {
      ethhdr->dest.addr[i] = dest->addr[i];
      ethhdr->src.addr[i] = srcaddr->addr[i];
    }
    ethhdr->type = htons(ETHTYPE_IP);
    /* send packet */
    result = netif->linkoutput(netif, q);
  }
  /* never reached; here for safety */
  pbuf_free(q);
  return result;
}

/**
 * Send an ARP request for the given IP address.
 *
 * If the IP address was not yet in the cache, a pending ARP cache entry
 * is added and an ARP request is sent for the given address. The packet
 * is queued on this entry.
 *
 * If the IP address was already pending in the cache, a new ARP request
 * is sent for the given address. The packet is queued on this entry.
 *
 * If the IP address was already stable in the cache, the packet is
 * directly sent. An ARP request is sent out.
 * 
 * @param netif The lwIP network interface where ipaddr
 * must be queried for.
 * @param ipaddr The IP address to be resolved.
 * @param q If non-NULL, a pbuf that must be delivered to the IP address.
 *
 * @return
 * - ERR_BUF Could not make room for Ethernet header.
 * - ERR_MEM Hardware address unknown, and no more ARP entries available
 *   to query for address or queue the packet.
 * - ERR_MEM Could not queue packet due to memory shortage.
 * - ERR_RTE No route to destination (no gateway to external networks).
  *
 * @note Might be used in the future by manual IP configuration
 * as well.
 *
 * TODO: use the ctime field to see how long ago an ARP request was sent,
 * possibly retry.
 */
err_t etharp_query(struct netif *netif, struct ip_addr *ipaddr, struct pbuf *q)
{
  struct pbuf *p;
  err_t result = ERR_OK;
  s8_t i; /* ARP entry index */
  u8_t k; /* Ethernet address octet index */

  /* Do three things in this order (by design):
   * 
   * 1) send out ARP request
   * 2) find entry in ARP cache
   * 3) handle the packet
   */

  /* allocate a pbuf for the outgoing ARP request packet */
  p = pbuf_alloc(PBUF_LINK, sizeof(struct etharp_hdr), PBUF_RAM);
  /* could allocate a pbuf for an ARP request? */
  if (p != NULL) {
    struct etharp_hdr *hdr = p->payload;
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_query: sending ARP request.\n"));
    hdr->opcode = htons(ARP_REQUEST);
    for (k = 0; k < netif->hwaddr_len; k++)
    {
      hdr->shwaddr.addr[k] = srcaddr->addr[k];
      /* the hardware address is what we ask for, in
       * a request it is a don't-care value, we use zeroes */
      hdr->dhwaddr.addr[k] = 0x00;
    }
    hdr->dipaddr = *(struct ip_addr2 *)ipaddr;
    hdr->sipaddr = *(struct ip_addr2 *)&netif->ip_addr;

    hdr->hwtype = htons(HWTYPE_ETHERNET);
    ARPH_HWLEN_SET(hdr, netif->hwaddr_len);

    hdr->proto = htons(ETHTYPE_IP);
    ARPH_PROTOLEN_SET(hdr, sizeof(struct ip_addr));
    for (k = 0; k < netif->hwaddr_len; ++k)
    {
      /* broadcast to all network interfaces on the local network */
      hdr->ethhdr.dest.addr[k] = 0xff;
      hdr->ethhdr.src.addr[k] = srcaddr->addr[k];
    }
    hdr->ethhdr.type = htons(ETHTYPE_ARP);
    /* send ARP query */
    result = netif->linkoutput(netif, p);
    /* free ARP query packet */
    pbuf_free(p);
    p = NULL;
  /* could not allocate pbuf for ARP request */
  } else {
    result = ERR_MEM;
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE | 2, ("etharp_query: could not allocate pbuf for ARP request.\n"));
  }

  /* search entry of queried IP address in the ARP cache */
  for (i = 0; i < ARP_TABLE_SIZE; ++i) {
    /* valid ARP cache entry with matching IP address? */
    if (arp_table[i].state != ETHARP_STATE_EMPTY &&
    	ip_addr_cmp(ipaddr, &arp_table[i].ipaddr)) {
      /* pending entry? */
      if (arp_table[i].state == ETHARP_STATE_PENDING) {
        LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE | DBG_STATE, ("etharp_query: requested IP already pending in entry %u\n", i));
        /* { i != ARP_TABLE_SIZE } */
        break;
      }
      else if (arp_table[i].state == ETHARP_STATE_STABLE) {
        LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE | DBG_STATE, ("etharp_query: requested IP already stable in entry %u\n", i));
        /* { i != ARP_TABLE_SIZE } */
        break;
      }
    }
  }
  /* queried address not yet in ARP table? */
  if (i == ARP_TABLE_SIZE) {
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_query: IP address not found in ARP table\n"));
    /* find an available (unused or old) entry */
    i = find_arp_entry();
    /* bail out if no ARP entries are available */
    if (i == ERR_MEM) {
      LWIP_DEBUGF(ETHARP_DEBUG | 2, ("etharp_query: no more ARP entries available. Should seldom occur.\n"));
      return ERR_MEM;
    }
    /* i is available, create ARP entry */
    arp_table[i].state = ETHARP_STATE_PENDING;
    ip_addr_set(&arp_table[i].ipaddr, ipaddr);
    arp_table[i].p = NULL;
    LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_query: added pending entry %u for IP address\n", i));
  }

  /* { i is either a (new or existing) PENDING or STABLE entry } */
  
  /* packet given? */
  if (q != NULL)
    /* stable entry? */
    if (arp_table[i].state == ETHARP_STATE_STABLE) {
      /* we have a valid IP->Ethernet address mapping,
       * fill in the Ethernet header for the outgoing packet */
      struct eth_addr * srcaddr = (struct eth_addr *)netif->hwaddr;
      struct eth_hdr *ethhdr = q->payload;
      for(k = 0; k < netif->hwaddr_len; k++) {
        ethhdr->dest.addr[k] = arp_table[i].ethaddr.addr[k];
        ethhdr->src.addr[k]  = srcaddr->addr[k];
      }
      ethhdr->type = htons(ETHTYPE_IP);
      LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_query: sending packet %p\n", (void *)q));
      /* send the packet */
      result = netif->linkoutput(netif, q);
#if ARP_QUEUEING /* queue the given q packet */
    /* pending entry? (either just created or already pending */
    } else if (arp_table[i].state == ETHARP_STATE_PENDING) {
      /* copy any PBUF_REF referenced payloads into PBUF_RAM */
      /* (the caller assumes the referenced payload can be freed) */
      p = pbuf_take(q);
      /* queue packet */
      if (p != NULL) {
        pbuf_queue(arp_table[i].p, p);
        LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_query: queued packet %p on ARP entry %d\n", (void *)q, i));
      } else {
        LWIP_DEBUGF(ETHARP_DEBUG | DBG_TRACE, ("etharp_query: could not queue a copy of PBUF_REF packet %p (out of memory)\n", (void *)q));
        result = ERR_MEM; 
      }
#endif
    }
  }
  return result;
}
