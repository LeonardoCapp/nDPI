/*
 * redis.c
 *
 * Copyright (C) 2011-22 - ntop.org
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_REDIS

#include "ndpi_api.h"


static void ndpi_int_redis_add_connection(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow) {
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_REDIS, NDPI_PROTOCOL_UNKNOWN);
}


static void ndpi_check_redis(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow) {
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  u_int32_t payload_len = packet->payload_packet_len;
  
  if(payload_len == 0) return; /* Shouldn't happen */

  /* Break after 20 packets. */
  if(flow->packet_counter > 20) {
    NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
    return;
  }

  if(packet->packet_direction == 0)
    flow->redis_s2d_first_char = packet->payload[0];
  else
    flow->redis_d2s_first_char = packet->payload[0];

  if((flow->redis_s2d_first_char != '\0') && (flow->redis_d2s_first_char != '\0')) {
    /*
     *1
     $4
     PING
     +PONG
     *3
     $3
     SET
     $19
     dns.cache.127.0.0.1
     $9
     localhost
     +OK
    */

    if(((flow->redis_s2d_first_char == '*') 
	&& ((flow->redis_d2s_first_char == '+') || (flow->redis_d2s_first_char == ':')))
       || ((flow->redis_d2s_first_char == '*') 
	   && ((flow->redis_s2d_first_char == '+') || (flow->redis_s2d_first_char == ':')))) {
      NDPI_LOG_INFO(ndpi_struct, "Found Redis\n");
      ndpi_int_redis_add_connection(ndpi_struct, flow);
    } else {
      NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
    }
  } else
    return; /* Too early */
}

void ndpi_search_redis(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow) {
  NDPI_LOG_DBG(ndpi_struct, "search Redis\n");

  /* skip marked packets */
  if (flow->detected_protocol_stack[0] != NDPI_PROTOCOL_REDIS) {
    ndpi_check_redis(ndpi_struct, flow);
  }
}


void init_redis_dissector(struct ndpi_detection_module_struct *ndpi_struct, u_int32_t *id, NDPI_PROTOCOL_BITMASK *detection_bitmask)
{
  ndpi_set_bitmask_protocol_detection("Redis", ndpi_struct, detection_bitmask, *id,
				      NDPI_PROTOCOL_REDIS,
				      ndpi_search_redis,
				      NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_TCP_WITH_PAYLOAD_WITHOUT_RETRANSMISSION,
				      SAVE_DETECTION_BITMASK_AS_UNKNOWN,
				      ADD_TO_DETECTION_BITMASK);

  *id += 1;
}
