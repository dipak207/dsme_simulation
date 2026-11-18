#include "contiki.h"
#include "contiki-lib.h"
#include "contiki-net.h"
#include "net/ip/uip.h"
#include "net/rpl/rpl.h"
#include "net/netstack.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/rpl/rpl-private.h"

#include "net/mac/dsme/dsme.h"

#include "lib/assert.h"

#if CONTIKI_TARGET_IOTLAB_M3
#include "../../../../openlab/drivers/unique_id.h"
#define IS_SERVER (PAN_COORDINATOR == platform_uid())
#elif CONTIKI_TARGET_COOJA
#include "node-id.h"
#define IS_SERVER (PAN_COORDINATOR == node_id)
#else
#error "Node ID not specified"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#define DEBUG DEBUG_FULL
#include "net/ip/uip-debug.h"

#include "dev/serial-line.h"

#define SEND_INTERVAL		((PERIOD * CLOCK_SECOND)/1000)
#define SEND_TIME		(random_rand() % (SEND_INTERVAL))
#define MAX_PAYLOAD_LEN		30
#ifndef APP_HIGH_DEADLINE_MS
#define APP_HIGH_DEADLINE_MS 0
#endif
#ifndef APP_LOW_DEADLINE_MS
#define APP_LOW_DEADLINE_MS 0
#endif
#ifndef APP_TRAFFIC_STOP_SECONDS
#define APP_TRAFFIC_STOP_SECONDS 290
#endif

#define UIP_IP_BUF   ((struct uip_ip_hdr *)&uip_buf[UIP_LLH_LEN])

#define UDP_CLIENT_PORT	8765
#define UDP_SERVER_PORT	5678

#define UDP_EXAMPLE_ID  190

#ifndef ADAPTIVE_MO_DEMO
#define ADAPTIVE_MO_DEMO 0
#endif

#ifndef ADAPTIVE_MO_PHASE_SECONDS
#define ADAPTIVE_MO_PHASE_SECONDS 30
#endif

#ifndef ADAPTIVE_MO_DEMAND_INTERVAL_MS
#define ADAPTIVE_MO_DEMAND_INTERVAL_MS 400
#endif

#ifndef ADAPTIVE_MO_DEMAND_SLOTS
#define ADAPTIVE_MO_DEMAND_SLOTS 2
#endif

/* Capture protocol state in the final two seconds of each fixed-duration run.
 * This is deliberately a near-termination snapshot: it leaves enough time for
 * all three motes to emit their records before the CSC ScriptRunner timeout. */
#ifndef DSME_FINAL_STATE_SNAPSHOT_SECONDS
#if ADAPTIVE_MO_DEMO
#define DSME_FINAL_STATE_SNAPSHOT_SECONDS 298
#else
#define DSME_FINAL_STATE_SNAPSHOT_SECONDS 118
#endif
#endif

static struct uip_udp_conn *connection;
static uip_ipaddr_t server_ipaddr;
static uint32_t seq_id;
static uint8_t gts_request_started;
static struct etimer demand_timer;
#if ADAPTIVE_MO_DEMO
static unsigned long logged_workload_phase = (unsigned long)-1;
#endif

/*---------------------------------------------------------------------------*/
static void
tcpip_handler(void)
{
  char *appdata;

  if(uip_newdata()) {
    appdata = (char *)uip_appdata;
    appdata[uip_datalen()] = 0;
    if(uip_datalen() > 0) {
      unsigned source, sequence, payload_len;
      char traffic_class;
      if(sscanf(appdata, "Q1:%u:%u:%c:%u", &source, &sequence, &traffic_class, &payload_len) == 4 &&
         payload_len == uip_datalen() && (traffic_class == 'H' || traffic_class == 'L')) {
        PRINTF("APP_PACKET_RX source_mote_id=%u sequence=%u traffic_class=%c payload_length=%u\n",
               source, sequence, traffic_class, payload_len);
      } else {
        PRINTF("APP_PACKET_RX_MALFORMED length=%u\n", uip_datalen());
      }
    } else {
      PRINTF("APP_PACKET_RX_MALFORMED length=%u\n", uip_datalen());
    }
    PRINTF("DATA recv '%s' from ", appdata);
    PRINTF("%d",
           UIP_IP_BUF->srcipaddr.u8[sizeof(UIP_IP_BUF->srcipaddr.u8) - 1]);
    PRINTF("\n");
  }
}
/*---------------------------------------------------------------------------*/
static void
send_packet(void *ptr)
{
  char buf[MAX_PAYLOAD_LEN];
  if(clock_seconds() >= APP_TRAFFIC_STOP_SECONDS) return;
#if CONTIKI_TARGET_COOJA
  const unsigned source = node_id;
#else
  const unsigned source = (unsigned)platform_uid();
#endif
  const unsigned sequence = (unsigned)seq_id++ ;
  const char traffic_class = ((clock_seconds() / ADAPTIVE_MO_PHASE_SECONDS) % 2) ? 'H' : 'L';
  const unsigned deadline_ms = traffic_class == 'H' ? APP_HIGH_DEADLINE_MS : APP_LOW_DEADLINE_MS;


  {
    unsigned payload_len = 0;
    unsigned actual_len;
    do {
      snprintf(buf, sizeof(buf), "Q1:%u:%u:%c:%u", source, sequence, traffic_class, payload_len);
      actual_len = (unsigned)strlen(buf);
      if(actual_len == payload_len) break;
      payload_len = actual_len;
    } while(1);
  }
  PRINTF("APP_PACKET_TX source_mote_id=%u sequence=%u traffic_class=%c payload_length=%u deadline_ms=%u\n",
         source, sequence, traffic_class, (unsigned)strlen(buf), deadline_ms);
  uip_udp_packet_sendto(connection, buf, strlen(buf),
                        &server_ipaddr, UIP_HTONS(UDP_SERVER_PORT));
}
/*---------------------------------------------------------------------------*/

PROCESS(main_process, "DSME Node");

AUTOSTART_PROCESSES(&main_process);

PROCESS_THREAD(main_process, ev, data)
{
  static struct etimer periodic;
  static struct etimer final_state_snapshot_timer;
  static struct ctimer backoff_timer;
  static uint8_t final_state_snapshot_logged;
  struct uip_ds6_addr *root_if;

  PROCESS_BEGIN();

  final_state_snapshot_logged = 0;
  PRINTF("APP_MEASUREMENT_WINDOW start_us=0 end_us=%lu\n", (unsigned long)APP_TRAFFIC_STOP_SECONDS * 1000000UL);
  etimer_set(&final_state_snapshot_timer,
             (clock_time_t)DSME_FINAL_STATE_SNAPSHOT_SECONDS * CLOCK_SECOND);

  uip_ip6addr(&server_ipaddr, UIP_DS6_DEFAULT_PREFIX, 0, 0, 0, 0, 0x00ff, 0xfe00, 1);

  PRINTF("PAN_COORDINATOR:%x max_nbr:%d max_routes:%d\n",
           PAN_COORDINATOR, NBR_TABLE_CONF_MAX_NEIGHBORS, UIP_CONF_MAX_ROUTES);

  if(IS_SERVER) {
    PRINTF("UDP server started\n");

    uip_ds6_addr_add(&server_ipaddr, 0, ADDR_MANUAL);
    root_if = uip_ds6_addr_lookup(&server_ipaddr);
    if(root_if != NULL) {
      rpl_dag_t *dag;
      dag = rpl_set_root(RPL_DEFAULT_INSTANCE,(uip_ip6addr_t *)&server_ipaddr);
      uip_ip6addr(&server_ipaddr, UIP_DS6_DEFAULT_PREFIX, 0, 0, 0, 0, 0, 0, 0);
      rpl_set_prefix(dag, &server_ipaddr, 64);
      PRINTF("created a new RPL dag\n");
    } else {
      PRINTF("failed to create a new RPL DAG\n");
    }

    connection = udp_new(NULL, UIP_HTONS(UDP_CLIENT_PORT), NULL);
	if(connection == NULL) {
		PRINTF("No UDP connection available, exiting the process!\n");
		PROCESS_EXIT();
	}
	udp_bind(connection, UIP_HTONS(UDP_SERVER_PORT));

	PRINTF("Created a server connection with remote address ");
	PRINT6ADDR(&connection->ripaddr);
	PRINTF(" local/remote port %u/%u\n", UIP_HTONS(connection->lport),
				 UIP_HTONS(connection->rport));
  }
  else {
    PRINTF("UDP client started\n");

    /* new connection with remote host */
    connection = udp_new(NULL, UIP_HTONS(UDP_SERVER_PORT), NULL); 
    if(connection == NULL) {
      PRINTF("No UDP connection available, exiting the process!\n");
      PROCESS_EXIT();
    }
    udp_bind(connection, UIP_HTONS(UDP_CLIENT_PORT)); 

    PRINTF("Created a connection with the server ");
    PRINT6ADDR(&connection->ripaddr);
    PRINTF(" local/remote port %u/%u\n",
	UIP_HTONS(connection->lport), UIP_HTONS(connection->rport));
  }

  etimer_set(&periodic, SEND_INTERVAL);
#if ADAPTIVE_MO_DEMO
  if(!IS_SERVER) {
    etimer_set(&demand_timer, (clock_time_t)ADAPTIVE_MO_DEMAND_INTERVAL_MS * CLOCK_SECOND / 1000);
  }
#endif
  while(1) {
    PROCESS_YIELD();

    if(!final_state_snapshot_logged && etimer_expired(&final_state_snapshot_timer)) {
      dsmeLogMOProtocolSnapshot();
      final_state_snapshot_logged = 1;
    }

    if(ev == tcpip_event) {
      tcpip_handler();
    }

#if ADAPTIVE_MO_DEMO
    if(!IS_SERVER && etimer_expired(&demand_timer)) {
      const unsigned long now_s = clock_seconds();
      const unsigned long phase = now_s / ADAPTIVE_MO_PHASE_SECONDS;
      const unsigned long phase_start_s = phase * ADAPTIVE_MO_PHASE_SECONDS;
      const unsigned long phase_end_s = phase_start_s + ADAPTIVE_MO_PHASE_SECONDS;
      const bool high_demand = (phase % 2 == 1) && phase_start_s < 240;

      if(phase != logged_workload_phase) {
        PRINTF("APP_LOAD_PHASE t=%lu phase=%lu demand=%s start_s=%lu end_s=%lu\n",
               now_s, phase, high_demand ? "HIGH" : "LOW", phase_start_s,
               phase_end_s > 300 ? 300 : phase_end_s);
        logged_workload_phase = phase;
      }

      if(high_demand && gts_request_started) {
        uint8_t accepted = dsmeRequestGTS(PAN_COORDINATOR, ADAPTIVE_MO_DEMAND_SLOTS);
        PRINTF("APP_GTS_DEMAND_INVOKE t=%lu phase=%lu slots=%u accepted=%u\n",
               now_s, phase, (unsigned)ADAPTIVE_MO_DEMAND_SLOTS, (unsigned)accepted);
      }
      etimer_set(&demand_timer, (clock_time_t)ADAPTIVE_MO_DEMAND_INTERVAL_MS * CLOCK_SECOND / 1000);
    }
#endif

    if(etimer_expired(&periodic)) {
      etimer_reset(&periodic);
      if(!IS_SERVER) {
        if(!gts_request_started && dsmeAssociated()) {
          PRINTF("APP association complete; requesting uplink GTS to %u\n", PAN_COORDINATOR);
          if(dsmeRequestGTS(PAN_COORDINATOR, 1)) {
            gts_request_started = 1;
          } else {
            PRINTF("APP GTS request was rejected before FSM acceptance\n");
          }
        }
        if(dsmeHasValidGTS(PAN_COORDINATOR)) {
          ctimer_set(&backoff_timer, SEND_TIME, send_packet, NULL);
        }
      }
      else {
          PRINTF(".\n");
      }
    }
  }

  PROCESS_END();
}

void
rpl_callback_parent_switch_print(rpl_parent_t *old, rpl_parent_t *new)
{
  PRINTF("new parent ");
  PRINT6ADDR(rpl_get_parent_ipaddr(new));
  PRINTF("\n");
}
