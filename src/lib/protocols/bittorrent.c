/*
 * bittorrent.c
 *
 * Copyright (C) 2009-11 - ipoque GmbH
 * Copyright (C) 2011-22 - ntop.org
 *
 * This file is part of nDPI, an open source deep packet inspection
 * library based on the OpenDPI and PACE technology by ipoque GmbH
 *
 * nDPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nDPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


#include "ndpi_protocol_ids.h"

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_BITTORRENT

#include "ndpi_api.h"

#define NDPI_PROTOCOL_UNSAFE_DETECTION 	 0
#define NDPI_PROTOCOL_SAFE_DETECTION 	 1

#define NDPI_PROTOCOL_PLAIN_DETECTION 	 0
#define NDPI_PROTOCOL_WEBSEED_DETECTION  2

#define BITTORRENT_PROTO_STRING          "BitTorrent protocol"

// #define BITTORRENT_CACHE_DEBUG 1

struct ndpi_utp_hdr {
  u_int8_t h_version:4, h_type:4, next_extension;
  u_int16_t connection_id;
  u_int32_t ts_usec, tdiff_usec, window_size;
  u_int16_t sequence_nr, ack_nr;
};

/* Forward declaration */
static void ndpi_search_bittorrent(struct ndpi_detection_module_struct *ndpi_struct,
				   struct ndpi_flow_struct *flow);
static void ndpi_search_bittorrent_hash(struct ndpi_detection_module_struct *ndpi_struct,
					struct ndpi_flow_struct *flow, int bt_offset);

/* *********************************************** */

static int search_bittorrent_again(struct ndpi_detection_module_struct *ndpi_struct,
				   struct ndpi_flow_struct *flow) {
  ndpi_search_bittorrent(ndpi_struct, flow);
  ndpi_search_bittorrent_hash(ndpi_struct, flow, -1);
  
  /* Possibly more processing */
  return(1);
}

/* *********************************************** */

static u_int8_t is_utpv1_pkt(const u_int8_t *payload, u_int payload_len) {
  struct ndpi_utp_hdr *h = (struct ndpi_utp_hdr*)payload;

  if(payload_len < sizeof(struct ndpi_utp_hdr)) return(0);
  if(h->h_version != 1)             return(0);
  if(h->h_type > 4)                 return(0);
  if(h->next_extension > 2)         return(0);
  if(ntohl(h->window_size) > 65565) return(0);

  if((h->window_size == 0) && (payload_len != sizeof(struct ndpi_utp_hdr)))
    return(0);

  return(1);
}

/* *********************************************** */

static void ndpi_search_bittorrent_hash(struct ndpi_detection_module_struct *ndpi_struct,
					struct ndpi_flow_struct *flow, int bt_offset) {
  const char *bt_hash = NULL; /* 20 bytes long */
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  
  if(bt_offset == -1) {
    const char *bt_magic = ndpi_strnstr((const char *)packet->payload,
					BITTORRENT_PROTO_STRING, packet->payload_packet_len);
    
    if(bt_magic) {
      if(bt_magic == (const char*)&packet->payload[1])
	bt_hash = (const char*)&packet->payload[28];
      else
	bt_hash = &bt_magic[19];
    }
  } else
    bt_hash = (const char*)&packet->payload[28];
  
  if(bt_hash && (packet->payload_packet_len >= (20 + (bt_hash-(const char*)packet->payload))))
    memcpy(flow->protos.bittorrent.hash, bt_hash, 20);
}

/* *********************************************** */

static void ndpi_add_connection_as_bittorrent(struct ndpi_detection_module_struct *ndpi_struct,
					      struct ndpi_flow_struct *flow,
					      int bt_offset, int check_hash,
					      const u_int8_t save_detection,
					      const u_int8_t encrypted_connection) {
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;

  if(check_hash)
    ndpi_search_bittorrent_hash(ndpi_struct, flow, bt_offset);

  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_BITTORRENT, NDPI_PROTOCOL_UNKNOWN);
  
  if(flow->protos.bittorrent.hash[0] == '\0') {
    /* This is necessary to inform the core to call this dissector again */
    flow->check_extra_packets = 1;
    
    /* Don't use just 1 as in TCP DNS more packets could be returned (e.g. ACK). */
    flow->max_extra_packets_to_check = 3;
    flow->extra_packets_func = search_bittorrent_again;
  }
  
  if(ndpi_struct->bittorrent_cache == NULL)
    ndpi_struct->bittorrent_cache = ndpi_lru_cache_init(32768);

  if(ndpi_struct->bittorrent_cache && packet->iph) {
    u_int32_t key1, key2, i;

    if(packet->udp)
      key1 = ndpi_bittorrent_hash_funct(packet->iph->saddr, packet->udp->source), key2 = ndpi_bittorrent_hash_funct(packet->iph->daddr, packet->udp->dest);
    else
      key1 = ndpi_bittorrent_hash_funct(packet->iph->saddr, packet->tcp->source), key2 = ndpi_bittorrent_hash_funct(packet->iph->daddr, packet->tcp->dest);
    
    ndpi_lru_add_to_cache(ndpi_struct->bittorrent_cache, key1, NDPI_PROTOCOL_BITTORRENT);
    ndpi_lru_add_to_cache(ndpi_struct->bittorrent_cache, key2, NDPI_PROTOCOL_BITTORRENT);

    /* Now add hosts as twins */
    ndpi_lru_add_to_cache(ndpi_struct->bittorrent_cache,
			  packet->iph->saddr + packet->iph->daddr,
			  NDPI_PROTOCOL_BITTORRENT);
    
    /* Also add +2 ports of the sender in order to catch additional sockets open by the same client */
    for(i=0; i<2; i++) {
      if(packet->udp)
	key1 = ndpi_bittorrent_hash_funct(packet->iph->saddr, htons(ntohs(packet->udp->source)+1));
      else
	key1 = ndpi_bittorrent_hash_funct(packet->iph->saddr, htons(ntohs(packet->tcp->source)+1));

      ndpi_lru_add_to_cache(ndpi_struct->bittorrent_cache, key1, NDPI_PROTOCOL_BITTORRENT);
    }
    
#ifdef BITTORRENT_CACHE_DEBUG
    if(packet->udp)
      printf("[BitTorrent] [UDP] *** ADDED ports %u / %u [%u][%u]\n", ntohs(packet->udp->source), ntohs(packet->udp->dest), key1, key2);
    else
      printf("[BitTorrent] [TCP] *** ADDED ports %u / %u [%u][%u]\n", ntohs(packet->tcp->source), ntohs(packet->tcp->dest), key1, key2);
#endif
  }
}

/* ************************************* */

static u_int8_t ndpi_int_search_bittorrent_tcp_zero(struct ndpi_detection_module_struct
						    *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  u_int16_t a = 0;

  if(packet->payload_packet_len == 1 && packet->payload[0] == 0x13) {
    /* reset stage back to 0 so we will see the next packet here too */
    flow->bittorrent_stage = 0;
    return 0;
  }

  if(flow->packet_counter == 2 && packet->payload_packet_len > 20) {
    if(memcmp(&packet->payload[0], BITTORRENT_PROTO_STRING, 19) == 0) {
      NDPI_LOG_INFO(ndpi_struct, "found BT: plain\n");
      ndpi_add_connection_as_bittorrent(ndpi_struct, flow, 19, 1,
					NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
      return 1;
    }
  }

  if(packet->payload_packet_len > 20) {
    /* test for match 0x13+BITTORRENT_PROTO_STRING */
    if(packet->payload[0] == 0x13) {
      if(memcmp(&packet->payload[1], BITTORRENT_PROTO_STRING, 19) == 0) {
	NDPI_LOG_INFO(ndpi_struct, "found BT: plain\n");
	ndpi_add_connection_as_bittorrent(ndpi_struct, flow, 20, 1,
					  NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
	return 1;
      }
    }
  }

  if(packet->payload_packet_len > 23 && memcmp(packet->payload, "GET /webseed?info_hash=", 23) == 0) {
    NDPI_LOG_INFO(ndpi_struct, "found BT: plain webseed\n");
    ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
				      NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_WEBSEED_DETECTION);
    return 1;
  }
  /* seen Azureus as server for webseed, possibly other servers existing, to implement */
  /* is Server: hypertracker Bittorrent? */
  /* no asymmetric detection possible for answer of pattern "GET /data?fid=". */
  if(packet->payload_packet_len > 60
     && memcmp(packet->payload, "GET /data?fid=", 14) == 0 && memcmp(&packet->payload[54], "&size=", 6) == 0) {
    NDPI_LOG_INFO(ndpi_struct, "found BT: plain Bitcomet persistent seed\n");
    ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
				      NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_WEBSEED_DETECTION);
    return 1;
  }


  if(packet->payload_packet_len > 90 && (memcmp(packet->payload, "GET ", 4) == 0
					 || memcmp(packet->payload, "POST ", 5) == 0)) {
    const u_int8_t *ptr = &packet->payload[4];
    u_int16_t len = packet->payload_packet_len - 4;

    /* parse complete get packet here into line structure elements */
    ndpi_parse_packet_line_info(ndpi_struct, flow);
    /* answer to this pattern is HTTP....Server: hypertracker */
    if(packet->user_agent_line.ptr != NULL
       && ((packet->user_agent_line.len > 8 && memcmp(packet->user_agent_line.ptr, "Azureus ", 8) == 0)
	   || (packet->user_agent_line.len >= 10 && memcmp(packet->user_agent_line.ptr, "BitTorrent", 10) == 0)
	   || (packet->user_agent_line.len >= 11 && memcmp(packet->user_agent_line.ptr, "BTWebClient", 11) == 0))) {
      NDPI_LOG_INFO(ndpi_struct, "found BT: Azureus /Bittorrent user agent\n");
      ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_WEBSEED_DETECTION);
      return 1;
    }

    if(packet->user_agent_line.ptr != NULL
       && (packet->user_agent_line.len >= 9 && memcmp(packet->user_agent_line.ptr, "Shareaza ", 9) == 0)
       && (packet->parsed_lines > 8 && packet->line[8].ptr != 0
	   && packet->line[8].len >= 9 && memcmp(packet->line[8].ptr, "X-Queue: ", 9) == 0)) {
      NDPI_LOG_INFO(ndpi_struct, "found BT: Shareaza detected\n");
      ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_WEBSEED_DETECTION);
      return 1;
    }

    /* this is a self built client, not possible to catch asymmetrically */
    if((packet->parsed_lines == 10 || (packet->parsed_lines == 11 && packet->line[10].len == 0))
       && packet->user_agent_line.ptr != NULL
       && packet->user_agent_line.len > 12
       && memcmp(packet->user_agent_line.ptr, "Mozilla/4.0 ",
		 12) == 0
       && packet->host_line.ptr != NULL
       && packet->host_line.len >= 7
       && packet->line[2].ptr != NULL
       && packet->line[2].len > 14
       && memcmp(packet->line[2].ptr, "Keep-Alive: 300", 15) == 0
       && packet->line[3].ptr != NULL
       && packet->line[3].len > 21
       && memcmp(packet->line[3].ptr, "Connection: Keep-alive", 22) == 0
       && packet->line[4].ptr != NULL
       && packet->line[4].len > 10
       && (memcmp(packet->line[4].ptr, "Accpet: */*", 11) == 0
	   || memcmp(packet->line[4].ptr, "Accept: */*", 11) == 0)

       && packet->line[5].ptr != NULL
       && packet->line[5].len > 12
       && memcmp(packet->line[5].ptr, "Range: bytes=", 13) == 0
       && packet->line[7].ptr != NULL
       && packet->line[7].len > 15
       && memcmp(packet->line[7].ptr, "Pragma: no-cache", 16) == 0
       && packet->line[8].ptr != NULL
       && packet->line[8].len > 22 && memcmp(packet->line[8].ptr, "Cache-Control: no-cache", 23) == 0) {

      NDPI_LOG_INFO(ndpi_struct, "found BT: Bitcomet LTS\n");
      ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					NDPI_PROTOCOL_UNSAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
      return 1;
    }

    /* FlashGet pattern */
    if(packet->parsed_lines == 8
       && packet->user_agent_line.ptr != NULL
       && packet->user_agent_line.len > (sizeof("Mozilla/4.0 (compatible; MSIE 6.0;") - 1)
       && memcmp(packet->user_agent_line.ptr, "Mozilla/4.0 (compatible; MSIE 6.0;",
		 sizeof("Mozilla/4.0 (compatible; MSIE 6.0;") - 1) == 0
       && packet->host_line.ptr != NULL
       && packet->host_line.len >= 7
       && packet->line[2].ptr != NULL
       && packet->line[2].len == 11
       && memcmp(packet->line[2].ptr, "Accept: */*", 11) == 0
       && packet->line[3].ptr != NULL && packet->line[3].len >= (sizeof("Referer: ") - 1)
       && memcmp(packet->line[3].ptr, "Referer: ", sizeof("Referer: ") - 1) == 0
       && packet->line[5].ptr != NULL
       && packet->line[5].len > 13
       && memcmp(packet->line[5].ptr, "Range: bytes=", 13) == 0
       && packet->line[6].ptr != NULL
       && packet->line[6].len > 21 && memcmp(packet->line[6].ptr, "Connection: Keep-Alive", 22) == 0) {

      NDPI_LOG_INFO(ndpi_struct, "found BT: FlashGet\n");
      ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					NDPI_PROTOCOL_UNSAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
      return 1;
    }

    if(packet->parsed_lines == 7
       && packet->user_agent_line.ptr != NULL
       && packet->user_agent_line.len > (sizeof("Mozilla/4.0 (compatible; MSIE 6.0;") - 1)
       && memcmp(packet->user_agent_line.ptr, "Mozilla/4.0 (compatible; MSIE 6.0;",
		 sizeof("Mozilla/4.0 (compatible; MSIE 6.0;") - 1) == 0
       && packet->host_line.ptr != NULL
       && packet->host_line.len >= 7
       && packet->line[2].ptr != NULL
       && packet->line[2].len == 11
       && memcmp(packet->line[2].ptr, "Accept: */*", 11) == 0
       && packet->line[3].ptr != NULL && packet->line[3].len >= (sizeof("Referer: ") - 1)
       && memcmp(packet->line[3].ptr, "Referer: ", sizeof("Referer: ") - 1) == 0
       && packet->line[5].ptr != NULL
       && packet->line[5].len > 21 && memcmp(packet->line[5].ptr, "Connection: Keep-Alive", 22) == 0) {

      NDPI_LOG_INFO(ndpi_struct, "found BT: FlashGet\n");
      ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					NDPI_PROTOCOL_UNSAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
      return 1;
    }

    /* answer to this pattern is not possible to implement asymmetrically */
    while (1) {
      if(len < 50 || ptr[0] == 0x0d) {
	goto ndpi_end_bt_tracker_check;
      }
      if(memcmp(ptr, "info_hash=", 10) == 0) {
	break;
      }
      len--;
      ptr++;
    }

    NDPI_LOG_DBG2(ndpi_struct, " BT stat: tracker info hash found\n");

    /* len is > 50, so save operation here */
    len -= 10;
    ptr += 10;

    /* parse bt hash */
    for (a = 0; a < 20; a++) {
      if(len < 3) {
	goto ndpi_end_bt_tracker_check;
      }
      if(*ptr == '%') {
	u_int8_t x1 = 0xFF;
	u_int8_t x2 = 0xFF;


	if(ptr[1] >= '0' && ptr[1] <= '9') {
	  x1 = ptr[1] - '0';
	}
	if(ptr[1] >= 'a' && ptr[1] <= 'f') {
	  x1 = 10 + ptr[1] - 'a';
	}
	if(ptr[1] >= 'A' && ptr[1] <= 'F') {
	  x1 = 10 + ptr[1] - 'A';
	}

	if(ptr[2] >= '0' && ptr[2] <= '9') {
	  x2 = ptr[2] - '0';
	}
	if(ptr[2] >= 'a' && ptr[2] <= 'f') {
	  x2 = 10 + ptr[2] - 'a';
	}
	if(ptr[2] >= 'A' && ptr[2] <= 'F') {
	  x2 = 10 + ptr[2] - 'A';
	}

	if(x1 == 0xFF || x2 == 0xFF) {
	  goto ndpi_end_bt_tracker_check;
	}
	ptr += 3;
	len -= 3;
      } else if(*ptr >= 32 && *ptr < 127) {
	ptr++;
	len--;
      } else {
	goto ndpi_end_bt_tracker_check;
      }
    }

    NDPI_LOG_INFO(ndpi_struct, "found BT: tracker info hash parsed\n");
    ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
				      NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
    return 1;
  }

 ndpi_end_bt_tracker_check:

  if(packet->payload_packet_len == 80) {
    /* Warez 80 Bytes Packet
     * +----------------+---------------+-----------------+-----------------+
     * |20 BytesPattern | 32 Bytes Value| 12 BytesPattern | 16 Bytes Data   |
     * +----------------+---------------+-----------------+-----------------+
     * 20 BytesPattern : 4c 00 00 00 ff ff ff ff 57 00 00 00 00 00 00 00 20 00 00 00
     * 12 BytesPattern : 28 23 00 00 01 00 00 00 10 00 00 00
     * */
    static const char pattern_20_bytes[20] = { 0x4c, 0x00, 0x00, 0x00, 0xff,
					       0xff, 0xff, 0xff, 0x57, 0x00,
					       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00
    };
    static const char pattern_12_bytes[12] = { 0x28, 0x23, 0x00, 0x00, 0x01,
					       0x00, 0x00, 0x00, 0x10, 0x00,
					       0x00, 0x00
    };

    /* did not see this pattern anywhere */
    if((memcmp(&packet->payload[0], pattern_20_bytes, 20) == 0)
       && (memcmp(&packet->payload[52], pattern_12_bytes, 12) == 0)) {
      NDPI_LOG_INFO(ndpi_struct, "found BT: Warez - Plain\n");
      ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
      return 1;
    }
  }

  else if(packet->payload_packet_len > 50) {
    if(memcmp(packet->payload, "GET", 3) == 0) {

      ndpi_parse_packet_line_info(ndpi_struct, flow);
      /* haven't fount this pattern anywhere */
      if(packet->host_line.ptr != NULL
	 && packet->host_line.len >= 9 && memcmp(packet->host_line.ptr, "ip2p.com:", 9) == 0) {
	NDPI_LOG_INFO(ndpi_struct, "found BT: Warez - Plain Host: ip2p.com: pattern\n");
	ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					  NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_WEBSEED_DETECTION);
	return 1;
      }
    }
  }
  return 0;
}

/* ************************************* */

/* Search for BitTorrent commands */
static void ndpi_int_search_bittorrent_tcp(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;

  if(packet->payload_packet_len == 0) {
    return;
  }

  if(flow->bittorrent_stage == 0 && packet->payload_packet_len != 0) {
    /* exclude stage 0 detection from next run */
    flow->bittorrent_stage = 1;
    if(ndpi_int_search_bittorrent_tcp_zero(ndpi_struct, flow) != 0) {
      NDPI_LOG_DBG2(ndpi_struct, "stage 0 has detected something, returning\n");
      return;
    }

    NDPI_LOG_DBG2(ndpi_struct, "stage 0 has no direct detection, fall through\n");
  }
  return;
}

/* ************************************* */

static u_int8_t is_port(u_int16_t a, u_int16_t b, u_int16_t what) {
  return(((what == a) || (what == b)) ? 1 : 0);
}

/* ************************************* */

static void ndpi_skip_bittorrent(struct ndpi_detection_module_struct *ndpi_struct,
				 struct ndpi_flow_struct *flow,
				 struct ndpi_packet_struct *packet) {
  u_int16_t sport, dport;

  if(packet->udp)
    sport = packet->udp->source, dport = packet->udp->dest;
  else
    sport = packet->tcp->source, dport = packet->tcp->dest;
  
  if(ndpi_search_into_bittorrent_cache(ndpi_struct, flow, packet->iph->saddr, sport, packet->iph->daddr, dport))
    ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 0,
				      NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
  else
    NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
}

/* ************************************* */

static void ndpi_search_bittorrent(struct ndpi_detection_module_struct *ndpi_struct,
				   struct ndpi_flow_struct *flow) {
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  char *bt_proto = NULL;

  /* This is broadcast */
  if(packet->iph) {
    if((packet->iph->saddr == 0xFFFFFFFF) || (packet->iph->daddr == 0xFFFFFFFF))
      goto exclude_bt;

    if(packet->udp) {
      u_int16_t sport = ntohs(packet->udp->source), dport = ntohs(packet->udp->dest);

      if(is_port(sport, dport, 3544) /* teredo */
	 || is_port(sport, dport, 5246) || is_port(sport, dport, 5247) /* CAPWAP */) {
      exclude_bt:
	NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
	return;
      }
    }
  }

  if(flow->detected_protocol_stack[0] != NDPI_PROTOCOL_BITTORRENT) {
    /* check for tcp retransmission here */
    
    if(packet->tcp != NULL) {
      ndpi_int_search_bittorrent_tcp(ndpi_struct, flow);
    } else if(packet->udp != NULL) {
      /* UDP */
      const char *bt_search  = "BT-SEARCH * HTTP/1.1\r\n";
      const char *bt_search1 = "d1:ad2:id20:";

      if((ntohs(packet->udp->source) < 1024)
	 || (ntohs(packet->udp->dest) < 1024) /* High ports only */) {
	ndpi_skip_bittorrent(ndpi_struct, flow, packet);
	return;
      }

      /*
	Check for uTP http://www.bittorrent.org/beps/bep_0029.html

	wireshark/epan/dissectors/packet-bt-utp.c
      */

      if(packet->payload_packet_len >= 23 /* min header size */) {
	if(
	   (strncmp((const char*)packet->payload, bt_search, strlen(bt_search)) == 0)
	   || (strncmp((const char*)packet->payload, bt_search1, strlen(bt_search1)) == 0)
	   ) {
	  ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 1,
					    NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
	  return;
	} else {
	  /* Check if this is protocol v0 */
	  u_int8_t v0_extension = packet->payload[17];
	  u_int8_t v0_flags     = packet->payload[18];

	  if(is_utpv1_pkt(packet->payload, packet->payload_packet_len)) {
	    bt_proto = ndpi_strnstr((const char *)&packet->payload[20], BITTORRENT_PROTO_STRING, packet->payload_packet_len-20);
	    goto bittorrent_found;
	  } else if((packet->payload[0]== 0x60)
		    && (packet->payload[1]== 0x0)
		    && (packet->payload[2]== 0x0)
		    && (packet->payload[3]== 0x0)
		    && (packet->payload[4]== 0x0)) {
	    /* Heuristic */
	    bt_proto = ndpi_strnstr((const char *)&packet->payload[20], BITTORRENT_PROTO_STRING, packet->payload_packet_len-20);
	    goto bittorrent_found;
	    /* CSGO/DOTA conflict */
	  } else if((v0_flags < 6 /* ST_NUM_STATES */) && (v0_extension < 3 /* EXT_NUM_EXT */)) {
	    u_int32_t ts = ntohl(*((u_int32_t*)&(packet->payload[4])));
	    u_int32_t now;

	    now = (u_int32_t)time(NULL);

	    if((ts < (now+86400)) && (ts > (now-86400))) {
	      bt_proto = ndpi_strnstr((const char *)&packet->payload[20], BITTORRENT_PROTO_STRING, packet->payload_packet_len-20);
	      goto bittorrent_found;
	    }
	  } else if(ndpi_strnstr((const char *)&packet->payload[20], BITTORRENT_PROTO_STRING, packet->payload_packet_len-20)
		    ) {
	    goto bittorrent_found;
	  }

	}
      }

      flow->bittorrent_stage++;

      if(flow->bittorrent_stage < 5) {
	/* We have detected bittorrent but we need to wait until we get a hash */

	if(packet->payload_packet_len > 19 /* min size */) {
	  if(ndpi_strnstr((const char *)packet->payload, ":target20:", packet->payload_packet_len)
	     || ndpi_strnstr((const char *)packet->payload, ":find_node1:", packet->payload_packet_len)
	     || ndpi_strnstr((const char *)packet->payload, "d1:ad2:id20:", packet->payload_packet_len)
	     || ndpi_strnstr((const char *)packet->payload, ":info_hash20:", packet->payload_packet_len)
	     || ndpi_strnstr((const char *)packet->payload, ":filter64", packet->payload_packet_len)
	     || ndpi_strnstr((const char *)packet->payload, "d1:rd2:id20:", packet->payload_packet_len)
	     || (bt_proto = ndpi_strnstr((const char *)packet->payload, BITTORRENT_PROTO_STRING, packet->payload_packet_len))
	     ) {
	  bittorrent_found:
	    if(bt_proto != NULL && ((u_int8_t *)&bt_proto[27] - packet->payload +
				    sizeof(flow->protos.bittorrent.hash)) < packet->payload_packet_len) {
	      memcpy(flow->protos.bittorrent.hash, &bt_proto[27], sizeof(flow->protos.bittorrent.hash));
	      flow->extra_packets_func = NULL; /* Nothing else to do */
	    }

	    NDPI_LOG_INFO(ndpi_struct, "found BT: plain\n");
	    ndpi_add_connection_as_bittorrent(ndpi_struct, flow, -1, 0,
					      NDPI_PROTOCOL_SAFE_DETECTION, NDPI_PROTOCOL_PLAIN_DETECTION);
	    return;
	  }
	}

	return;
      }

      ndpi_skip_bittorrent(ndpi_struct, flow, packet);
    }
  }

  if(flow->packet_counter > 8) {
    ndpi_skip_bittorrent(ndpi_struct, flow, packet);
  }
}

/* ************************************* */

void init_bittorrent_dissector(struct ndpi_detection_module_struct *ndpi_struct,
			       u_int32_t *id, NDPI_PROTOCOL_BITMASK *detection_bitmask)
{
  ndpi_set_bitmask_protocol_detection("BitTorrent", ndpi_struct, detection_bitmask, *id,
				      NDPI_PROTOCOL_BITTORRENT,
				      ndpi_search_bittorrent,
				      NDPI_SELECTION_BITMASK_PROTOCOL_TCP_OR_UDP_WITH_PAYLOAD_WITHOUT_RETRANSMISSION,
				      SAVE_DETECTION_BITMASK_AS_UNKNOWN,
				      ADD_TO_DETECTION_BITMASK);
  *id += 1;
}
