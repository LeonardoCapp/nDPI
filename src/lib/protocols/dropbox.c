/*
 * dropbox.c
 *
 * Copyright (C) 2012-18 by ntop.org
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

#define NDPI_CURRENT_PROTO NDPI_PROTOCOL_DROPBOX

#include "ndpi_api.h"


#define DB_LSP_PORT 17500

static void ndpi_int_dropbox_add_connection(struct ndpi_detection_module_struct *ndpi_struct,
					    struct ndpi_flow_struct *flow,
					    u_int8_t due_to_correlation) {
  ndpi_set_detected_protocol(ndpi_struct, flow, NDPI_PROTOCOL_DROPBOX, NDPI_PROTOCOL_UNKNOWN);
}


static void ndpi_check_dropbox(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  struct ndpi_packet_struct *packet = &ndpi_struct->packet;
  // const u_int8_t *packet_payload = packet->payload;
  u_int32_t payload_len = packet->payload_packet_len;

  if(packet->udp != NULL) {
    u_int16_t dropbox_port = htons(DB_LSP_PORT);

    if(packet->udp->dest == dropbox_port) {    
      if(packet->udp->source == dropbox_port) {	
	if(payload_len > 10) {
	  if(ndpi_strnstr((const char *)packet->payload, "\"host_int\"", payload_len) != NULL) {	    
	    NDPI_LOG_INFO(ndpi_struct, "found dropbox\n");
	    ndpi_int_dropbox_add_connection(ndpi_struct, flow, 0);
	    return;
	  }
	}
      } else {
	if(payload_len > 10) {
	  if(ndpi_strnstr((const char *)packet->payload, "Bus17Cmd", payload_len) != NULL) {	    
	    NDPI_LOG_INFO(ndpi_struct, "found dropbox\n");
	    ndpi_int_dropbox_add_connection(ndpi_struct, flow, 0);
	    return;
	  }
	}
      }
    }
  }
  
  NDPI_EXCLUDE_PROTO(ndpi_struct, flow);
}

void ndpi_search_dropbox(struct ndpi_detection_module_struct *ndpi_struct, struct ndpi_flow_struct *flow)
{
  NDPI_LOG_DBG(ndpi_struct, "search dropbox\n");

  /* skip marked packets */
  if (flow->detected_protocol_stack[0] != NDPI_PROTOCOL_DROPBOX) {
    ndpi_check_dropbox(ndpi_struct, flow);
  }
}


void init_dropbox_dissector(struct ndpi_detection_module_struct *ndpi_struct, u_int32_t *id, NDPI_PROTOCOL_BITMASK *detection_bitmask) 
{
  ndpi_set_bitmask_protocol_detection("DROPBOX", ndpi_struct, detection_bitmask, *id,
				      NDPI_PROTOCOL_DROPBOX,
				      ndpi_search_dropbox,
				      NDPI_SELECTION_BITMASK_PROTOCOL_V4_V6_UDP_WITH_PAYLOAD,
				      SAVE_DETECTION_BITMASK_AS_UNKNOWN,
				      ADD_TO_DETECTION_BITMASK);
  *id += 1;
}
