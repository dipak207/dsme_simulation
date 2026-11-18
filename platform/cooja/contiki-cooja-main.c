/*
 * Copyright (c) 2010, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/**
 * \file
 *         COOJA Contiki mote main file.
 * \author
 *         Fredrik Osterlind <fros@sics.se>
 */

#define _GNU_SOURCE
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <link.h>

#include "contiki.h"
#include "sys/cc.h"

#include "sys/clock.h"
#include "sys/etimer.h"
#include "sys/cooja_mt.h"
#include "sys/autostart.h"

#include "lib/random.h"
#include "lib/simEnvChange.h"

#include "net/rime/rime.h"
#include "net/netstack.h"

#include "dev/eeprom.h"
#include "dev/serial-line.h"
#include "dev/cooja-radio.h"
#include "dev/button-sensor.h"
#include "dev/pir-sensor.h"
#include "dev/vib-sensor.h"

#include "sys/node-id.h"


/* JNI-defined functions, depends on the environment variable CLASSNAME */
#ifndef CLASSNAME
#error CLASSNAME is undefined, required by contiki-cooja-main.c
#endif /* CLASSNAME */
#define COOJA__QUOTEME(a,b,c) COOJA_QUOTEME(a,b,c)
#define COOJA_QUOTEME(a,b,c) a##b##c
#define COOJA_JNI_PATH Java_org_contikios_cooja_corecomm_
#define Java_org_contikios_cooja_corecomm_CLASSNAME_init COOJA__QUOTEME(COOJA_JNI_PATH,CLASSNAME,_init)
#define Java_org_contikios_cooja_corecomm_CLASSNAME_getMemory COOJA__QUOTEME(COOJA_JNI_PATH,CLASSNAME,_getMemory)
#define Java_org_contikios_cooja_corecomm_CLASSNAME_setMemory COOJA__QUOTEME(COOJA_JNI_PATH,CLASSNAME,_setMemory)
#define Java_org_contikios_cooja_corecomm_CLASSNAME_tick COOJA__QUOTEME(COOJA_JNI_PATH,CLASSNAME,_tick)
#define Java_org_contikios_cooja_corecomm_CLASSNAME_setReferenceAddress COOJA__QUOTEME(COOJA_JNI_PATH,CLASSNAME,_setReferenceAddress)
#define Java_org_contikios_cooja_corecomm_CLASSNAME_setMemorySections COOJA__QUOTEME(COOJA_JNI_PATH,CLASSNAME,_setMemorySections)

#ifndef NETSTACK_CONF_WITH_IPV4
#define NETSTACK_CONF_WITH_IPV4 0
#endif
#if NETSTACK_CONF_WITH_IPV4
#include "dev/rs232.h"
#include "dev/slip.h"
#include "net/ip/uip.h"
#include "net/ipv4/uip-fw.h"
#include "net/uip-fw-drv.h"
#include "net/ipv4/uip-over-mesh.h"
static struct uip_fw_netif slipif =
  {UIP_FW_NETIF(0,0,0,0, 255,255,255,255, slip_send)};
static struct uip_fw_netif meshif =
  {UIP_FW_NETIF(172,16,0,0, 255,255,0,0, uip_over_mesh_send)};

#define UIP_OVER_MESH_CHANNEL 8
static uint8_t is_gateway;
#endif /* NETSTACK_CONF_WITH_IPV4 */

#ifndef NETSTACK_CONF_WITH_IPV6
#define NETSTACK_CONF_WITH_IPV6 0
#endif
#if NETSTACK_CONF_WITH_IPV6
#include "net/ip/uip.h"
#include "net/ipv6/uip-ds6.h"
#define PRINT6ADDR(addr) printf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x", ((uint8_t *)addr)[0], ((uint8_t *)addr)[1], ((uint8_t *)addr)[2], ((uint8_t *)addr)[3], ((uint8_t *)addr)[4], ((uint8_t *)addr)[5], ((uint8_t *)addr)[6], ((uint8_t *)addr)[7], ((uint8_t *)addr)[8], ((uint8_t *)addr)[9], ((uint8_t *)addr)[10], ((uint8_t *)addr)[11], ((uint8_t *)addr)[12], ((uint8_t *)addr)[13], ((uint8_t *)addr)[14], ((uint8_t *)addr)[15])
#endif /* NETSTACK_CONF_WITH_IPV6 */

/* Simulation mote interfaces */
SIM_INTERFACE_NAME(moteid_interface);
SIM_INTERFACE_NAME(vib_interface);
SIM_INTERFACE_NAME(rs232_interface);
SIM_INTERFACE_NAME(simlog_interface);
SIM_INTERFACE_NAME(beep_interface);
SIM_INTERFACE_NAME(radio_interface);
SIM_INTERFACE_NAME(button_interface);
SIM_INTERFACE_NAME(pir_interface);
SIM_INTERFACE_NAME(clock_interface);
SIM_INTERFACE_NAME(leds_interface);
SIM_INTERFACE_NAME(cfs_interface);
SIM_INTERFACE_NAME(eeprom_interface);
SIM_INTERFACES(&vib_interface, &moteid_interface, &rs232_interface, &simlog_interface, &beep_interface, &radio_interface, &button_interface, &pir_interface, &clock_interface, &leds_interface, &cfs_interface, &eeprom_interface);
/* Example: manually add mote interfaces */
//SIM_INTERFACE_NAME(dummy_interface);
//SIM_INTERFACES(..., &dummy_interface);

/* Sensors */
SENSORS(&button_sensor, &pir_sensor, &vib_sensor);

/*
 * referenceVar is used for comparing absolute and process relative memory.
 * (this must not be static due to memory locations)
 */
long referenceVar;

static uintptr_t coojaObjectBase;
static uintptr_t coojaDataStartRel, coojaDataEndRel;
static uintptr_t coojaBssStartRel, coojaBssEndRel;
static int coojaMemorySectionsConfigured;

struct CoojaPhdrLookup {
  uintptr_t start;
  uintptr_t end;
  uintptr_t module_base;
  int required_flags;
  const char *module_name;
  uintptr_t segment_start;
  uintptr_t segment_end;
  unsigned int segment_flags;
};

static int
coojaFindObject(struct dl_phdr_info *info, size_t size, void *opaque)
{
  struct CoojaPhdrLookup *lookup = (struct CoojaPhdrLookup *)opaque;
  unsigned int i;
  (void)size;
  for(i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
    uintptr_t seg_start, seg_end;
    if(phdr->p_type != PT_LOAD || info->dlpi_addr > UINTPTR_MAX - phdr->p_vaddr) {
      continue;
    }
    seg_start = (uintptr_t)info->dlpi_addr + (uintptr_t)phdr->p_vaddr;
    if(phdr->p_memsz > UINTPTR_MAX - seg_start) {
      continue;
    }
    seg_end = seg_start + (uintptr_t)phdr->p_memsz;
    if(lookup->start >= seg_start && lookup->start < seg_end) {
      lookup->module_base = (uintptr_t)info->dlpi_addr;
      lookup->module_name = info->dlpi_name;
      lookup->segment_start = seg_start;
      lookup->segment_end = seg_end;
      lookup->segment_flags = phdr->p_flags;
      return 1;
    }
  }
  return 0;
}

static int
coojaFindSegment(struct dl_phdr_info *info, size_t size, void *opaque)
{
  struct CoojaPhdrLookup *lookup = (struct CoojaPhdrLookup *)opaque;
  unsigned int i;
  (void)size;
  if((uintptr_t)info->dlpi_addr != lookup->module_base) {
    return 0;
  }
  for(i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
    uintptr_t seg_start, seg_end;
    if(phdr->p_type != PT_LOAD || info->dlpi_addr > UINTPTR_MAX - phdr->p_vaddr) {
      continue;
    }
    seg_start = (uintptr_t)info->dlpi_addr + (uintptr_t)phdr->p_vaddr;
    if(phdr->p_memsz > UINTPTR_MAX - seg_start) {
      continue;
    }
    seg_end = seg_start + (uintptr_t)phdr->p_memsz;
    if(lookup->start >= seg_start && lookup->end <= seg_end
        && (phdr->p_flags & lookup->required_flags) == lookup->required_flags) {
      lookup->segment_start = seg_start;
      lookup->segment_end = seg_end;
      lookup->segment_flags = phdr->p_flags;
      lookup->module_name = info->dlpi_name;
      return 1;
    }
  }
  return 0;
}

static void
coojaThrow(JNIEnv *env, const char *message)
{
  jclass exceptionClass = (*env)->FindClass(env, "java/lang/IllegalStateException");
  if(exceptionClass != NULL) {
    (*env)->ThrowNew(env, exceptionClass, message);
  }
}

static int
coojaComputeAddress(JNIEnv *env, jint rel_addr, jint length,
                    uintptr_t *relative, uintptr_t *absolute)
{
  uintptr_t base = (uintptr_t)(unsigned long)referenceVar;
  if(!coojaMemorySectionsConfigured) {
    coojaThrow(env, "Cooja JNI memory section ranges were not configured");
    return 0;
  }
  if(rel_addr < 0 || length <= 0) {
    coojaThrow(env, "Cooja JNI memory request has a negative address or non-positive length");
    return 0;
  }
  *relative = (uintptr_t)(uint32_t)rel_addr;
  if(base > UINTPTR_MAX - *relative) {
    coojaThrow(env, "Cooja JNI relocation address addition overflow");
    return 0;
  }
  *absolute = base + *relative;
  if((uintptr_t)length > UINTPTR_MAX - *absolute) {
    coojaThrow(env, "Cooja JNI memory range end overflow");
    return 0;
  }
  return 1;
}

static int
coojaValidateMemoryRange(JNIEnv *env, jint rel_addr, jint length, int writing,
                         uintptr_t *absolute, const char **sectionName,
                         uintptr_t *sectionStart, uintptr_t *sectionEnd,
                         struct CoojaPhdrLookup *mapping)
{
  uintptr_t relative, end;
  int required_flags = PF_R | (writing ? PF_W : 0);
  if(!coojaComputeAddress(env, rel_addr, length, &relative, absolute)) {
    return 0;
  }
  if((uintptr_t)length > UINTPTR_MAX - relative) {
    coojaThrow(env, "Cooja JNI relative memory range end overflow");
    return 0;
  }
  end = relative + (uintptr_t)length;
  if(relative >= coojaDataStartRel && end <= coojaDataEndRel) {
    *sectionName = ".data";
    *sectionStart = coojaDataStartRel;
    *sectionEnd = coojaDataEndRel;
  } else if(relative >= coojaBssStartRel && end <= coojaBssEndRel) {
    *sectionName = ".bss";
    *sectionStart = coojaBssStartRel;
    *sectionEnd = coojaBssEndRel;
  } else {
    fprintf(stderr, "COOJA_JNI_RANGE_INVALID op=%s rel=0x%x len=%d referenceVar=0x%lx reason=outside-configured-data-bss\n",
            writing ? "write" : "read", (unsigned int)rel_addr,
            (int)length, referenceVar);
    coojaThrow(env, "Cooja JNI memory range is outside the configured .data/.bss section");
    return 0;
  }
  memset(mapping, 0, sizeof(*mapping));
  mapping->start = *absolute;
  mapping->end = *absolute + (uintptr_t)length;
  mapping->module_base = coojaObjectBase;
  mapping->required_flags = required_flags;
  if(dl_iterate_phdr(coojaFindSegment, mapping) != 1) {
    fprintf(stderr, "COOJA_JNI_RANGE_INVALID op=%s rel=0x%x len=%d referenceVar=0x%lx abs=0x%" PRIxPTR " section=%s sectionRel=[0x%" PRIxPTR ",0x%" PRIxPTR ") reason=not-contained-in-one-PT_LOAD-required-flags=%s\n",
            writing ? "write" : "read", (unsigned int)rel_addr,
            (int)length, referenceVar, *absolute, *sectionName, *sectionStart,
            *sectionEnd, writing ? "PF_R|PF_W" : "PF_R");
    coojaThrow(env, "Cooja JNI memory range is outside a single permitted PT_LOAD segment");
    return 0;
  }
  return 1;
}

/*
 * Contiki and rtimer threads.
 */
static struct cooja_mt_thread rtimer_thread;
static struct cooja_mt_thread process_run_thread;

/*---------------------------------------------------------------------------*/
#if NETSTACK_CONF_WITH_IPV4
static void
set_gateway(void)
{
  if(!is_gateway) {
    printf("%d.%d: making myself the IP network gateway.\n\n",
       linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
    printf("IPv4 address of the gateway: %d.%d.%d.%d\n\n",
       uip_ipaddr_to_quad(&uip_hostaddr));
    uip_over_mesh_set_gateway(&linkaddr_node_addr);
    uip_over_mesh_make_announced_gateway();
    is_gateway = 1;
  }
}
#endif /* NETSTACK_CONF_WITH_IPV4 */
/*---------------------------------------------------------------------------*/
static void
print_processes(struct process * const processes[])
{
  /*  const struct process * const * p = processes;*/
  printf("Starting");
  while(*processes != NULL) {
    printf(" '%s'", (*processes)->name);
    processes++;
  }
  putchar('\n');
}
/*---------------------------------------------------------------------------*/
static void
rtimer_thread_loop(void *data)
{
  while(1)
  {
    rtimer_arch_check();

    /* Return to COOJA */
    cooja_mt_yield();
  }
}
/*---------------------------------------------------------------------------*/
static void
set_rime_addr(void)
{
  linkaddr_t addr;
  int i;

  memset(&addr, 0, sizeof(linkaddr_t));
#if NETSTACK_CONF_WITH_IPV6
  for(i = 0; i < sizeof(uip_lladdr.addr); i += 2) {
    addr.u8[i + 1] = node_id & 0xff;
    addr.u8[i + 0] = node_id >> 8;
  }
#else /* NETSTACK_CONF_WITH_IPV6 */
  addr.u8[0] = node_id & 0xff;
  addr.u8[1] = node_id >> 8;
#endif /* NETSTACK_CONF_WITH_IPV6 */
  linkaddr_set_node_addr(&addr);
  printf("Rime started with address ");
  for(i = 0; i < sizeof(addr.u8) - 1; i++) {
    printf("%d.", addr.u8[i]);
  }
  printf("%d\n", addr.u8[i]);
}
/*---------------------------------------------------------------------------*/
void
contiki_init()
{
  /* Initialize random generator (moved to moteid.c) */

  /* Start process handler */
  process_init();


  /* Start Contiki processes */

  process_start(&etimer_process, NULL);
  process_start(&sensors_process, NULL);
  ctimer_init();

  /* Print startup information */
  printf(CONTIKI_VERSION_STRING " started. ");
  if(node_id > 0) {
    printf("Node id is set to %u.\n", node_id);
  } else {
    printf("Node id is not set.\n");
  }

  set_rime_addr();
  {
    uint8_t longaddr[8];

    memset(longaddr, 0, sizeof(longaddr));
    linkaddr_copy((linkaddr_t *)&longaddr, &linkaddr_node_addr);
    printf("MAC %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x ",
           longaddr[0], longaddr[1], longaddr[2], longaddr[3],
           longaddr[4], longaddr[5], longaddr[6], longaddr[7]);
  }

  queuebuf_init();

  /* Initialize communication stack */
  netstack_init();
  printf("%s/%s/%s, channel check rate %lu Hz\n",
         NETSTACK_NETWORK.name, NETSTACK_MAC.name, NETSTACK_RDC.name,
         CLOCK_SECOND / (NETSTACK_RDC.channel_check_interval() == 0 ? 1:
                         NETSTACK_RDC.channel_check_interval()));

#if NETSTACK_CONF_WITH_IPV4
  /* IPv4 CONFIGURATION */
  {
    uip_ipaddr_t hostaddr, netmask;

    process_start(&tcpip_process, NULL);
    process_start(&uip_fw_process, NULL);
    process_start(&slip_process, NULL);

    slip_set_input_callback(set_gateway);

    uip_init();
    uip_fw_init();
    uip_ipaddr(&hostaddr, 172,16,linkaddr_node_addr.u8[0],linkaddr_node_addr.u8[1]);
    uip_ipaddr(&netmask, 255,255,0,0);
    uip_ipaddr_copy(&meshif.ipaddr, &hostaddr);

    uip_sethostaddr(&hostaddr);
    uip_setnetmask(&netmask);
    uip_over_mesh_set_net(&hostaddr, &netmask);
    uip_over_mesh_set_gateway_netif(&slipif);
    uip_fw_default(&meshif);
    uip_over_mesh_init(UIP_OVER_MESH_CHANNEL);

    rs232_set_input(slip_input_byte);
    printf("IPv4 address: %d.%d.%d.%d\n", uip_ipaddr_to_quad(&hostaddr));
  }
#endif /* NETSTACK_CONF_WITH_IPV4 */

#if NETSTACK_CONF_WITH_IPV6
  /* IPv6 CONFIGURATION */
  {
    int i;
    uint8_t addr[sizeof(uip_lladdr.addr)];
    for(i = 0; i < sizeof(uip_lladdr.addr); i += 2) {
      addr[i + 1] = node_id & 0xff;
      addr[i + 0] = node_id >> 8;
    }
    linkaddr_copy((linkaddr_t *)addr, &linkaddr_node_addr);
    memcpy(&uip_lladdr.addr, addr, sizeof(uip_lladdr.addr));

    process_start(&tcpip_process, NULL);

    printf("Tentative link-local IPv6 address ");
    {
      uip_ds6_addr_t *lladdr;
      int i;
      lladdr = uip_ds6_get_link_local(-1);
      for(i = 0; i < 7; ++i) {
        printf("%02x%02x:", lladdr->ipaddr.u8[i * 2],
               lladdr->ipaddr.u8[i * 2 + 1]);
      }
      printf("%02x%02x\n", lladdr->ipaddr.u8[14],
             lladdr->ipaddr.u8[15]);
    }

    if(1) {
      uip_ipaddr_t ipaddr;
      int i;
      uip_ip6addr(&ipaddr, UIP_DS6_DEFAULT_PREFIX, 0, 0, 0, 0, 0, 0, 0);
      uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
      uip_ds6_addr_add(&ipaddr, 0, ADDR_TENTATIVE);
      printf("Tentative global IPv6 address ");
      for(i = 0; i < 7; ++i) {
        printf("%02x%02x:",
               ipaddr.u8[i * 2], ipaddr.u8[i * 2 + 1]);
      }
      printf("%02x%02x\n",
             ipaddr.u8[7 * 2], ipaddr.u8[7 * 2 + 1]);
    }
  }
#endif /* NETSTACK_CONF_WITH_IPV6 */

  /* Initialize eeprom */
  eeprom_init();

  /* Start serial process */
  serial_line_init();

  /* Start autostart processes (defined in Contiki application) */
  print_processes(autostart_processes);
  autostart_start(autostart_processes);
}
/*---------------------------------------------------------------------------*/
static void
process_run_thread_loop(void *data)
{
    /* Yield once during bootup */
    simProcessRunValue = 1;
    cooja_mt_yield();

    contiki_init();

    while(1)
    {
        simProcessRunValue = process_run();
        while(simProcessRunValue-- > 0) {
          process_run();
        }
        simProcessRunValue = process_nevents();

        /* Check if we must stay awake */
        if(simDontFallAsleep) {
            simDontFallAsleep=0;
            simProcessRunValue = 1;
        }
        /* Return to COOJA */
        cooja_mt_yield();
    }
}
/*---------------------------------------------------------------------------*/
/**
 * \brief      Initialize a mote by starting processes etc.
 * \param env  JNI Environment interface pointer
 * \param obj  unused
 *
 *             This function initializes a mote by starting certain
 *             processes and setting up the environment.
 *
 *             This is a JNI function and should only be called via the
 *             responsible Java part (MoteType.java).
 */
JNIEXPORT void JNICALL
Java_org_contikios_cooja_corecomm_CLASSNAME_init(JNIEnv *env, jobject obj)
{
  /* Create rtimers and Contiki threads */
  cooja_mt_start(&rtimer_thread, &rtimer_thread_loop, NULL);
  cooja_mt_start(&process_run_thread, &process_run_thread_loop, NULL);
 }
/*---------------------------------------------------------------------------*/
/**
 * \brief      Get a segment from the process memory.
 * \param env      JNI Environment interface pointer
 * \param obj      unused
 * \param rel_addr Start address of segment
 * \param length   Size of memory segment
 * \param mem_arr  Byte array destination for the fetched memory segment
 * \return     Java byte array containing a copy of memory segment.
 *
 *             Fetches a memory segment from the process memory starting at
 *             (rel_addr), with size (length). This function does not perform
 *             ANY error checking, and the process may crash if addresses are
 *             not available/readable.
 *
 *             This is a JNI function and should only be called via the
 *             responsible Java part (MoteType.java).
 */
JNIEXPORT void JNICALL
Java_org_contikios_cooja_corecomm_CLASSNAME_getMemory(JNIEnv *env, jobject obj, jint rel_addr, jint length, jbyteArray mem_arr)
{
  uintptr_t absolute;
  uintptr_t section_start, section_end;
  const char *section_name;
  struct CoojaPhdrLookup mapping;
  jsize array_length;
  (void)obj;
  (void)obj;
  if((*env)->ExceptionCheck(env)) {
    return;
  }
  if(mem_arr == NULL) {
    coojaThrow(env, "Cooja JNI getMemory received a null Java array");
    return;
  }
  array_length = (*env)->GetArrayLength(env, mem_arr);
  if((*env)->ExceptionCheck(env)) {
    return;
  }
  if(array_length != length) {
    fprintf(stderr, "COOJA_JNI_ARRAY_INVALID op=read rel=0x%x requested=%d arrayLength=%d\n",
            (unsigned int)rel_addr, (int)length, (int)array_length);
    coojaThrow(env, "Cooja JNI getMemory array length does not match requested length");
    return;
  }
  if(!coojaValidateMemoryRange(env, rel_addr, length, 0, &absolute,
                               &section_name, &section_start, &section_end, &mapping)) {
    return;
  }
  (*env)->SetByteArrayRegion(env, mem_arr, 0, (jsize)length, (const jbyte *)absolute);
  if((*env)->ExceptionCheck(env)) {
    fprintf(stderr, "COOJA_JNI_READ_EXCEPTION rel=0x%x len=%d abs=0x%" PRIxPTR "\n",
            (unsigned int)rel_addr, (int)length, absolute);
  }
}
/*---------------------------------------------------------------------------*/
/**
 * \brief      Replace a segment of the process memory with given byte array.
 * \param env      JNI Environment interface pointer
 * \param obj      unused
 * \param rel_addr Start address of segment
 * \param length   Size of memory segment
 * \param mem_arr  Byte array contaning new memory
 *
 *             Replaces a process memory segment with given byte array.
 *             This function does not perform ANY error checking, and the
 *             process may crash if addresses are not available/writable.
 *
 *             This is a JNI function and should only be called via the
 *             responsible Java part (MoteType.java).
 */
JNIEXPORT void JNICALL
Java_org_contikios_cooja_corecomm_CLASSNAME_setMemory(JNIEnv *env, jobject obj, jint rel_addr, jint length, jbyteArray mem_arr)
{
  uintptr_t absolute;
  uintptr_t section_start, section_end;
  const char *section_name;
  struct CoojaPhdrLookup mapping;
  jbyte *mem;
  jsize array_length;
  (void)obj;
  (void)obj;
  if((*env)->ExceptionCheck(env)) {
    return;
  }
  if(mem_arr == NULL) {
    coojaThrow(env, "Cooja JNI setMemory received a null Java array");
    return;
  }
  array_length = (*env)->GetArrayLength(env, mem_arr);
  if((*env)->ExceptionCheck(env)) {
    return;
  }
  if(array_length != length) {
    fprintf(stderr, "COOJA_JNI_ARRAY_INVALID op=write rel=0x%x requested=%d arrayLength=%d\n",
            (unsigned int)rel_addr, (int)length, (int)array_length);
    coojaThrow(env, "Cooja JNI setMemory array length does not match requested length");
    return;
  }
  if(!coojaValidateMemoryRange(env, rel_addr, length, 1, &absolute,
                               &section_name, &section_start, &section_end, &mapping)) {
    return;
  }
  mem = (*env)->GetByteArrayElements(env, mem_arr, NULL);
  if(mem == NULL || (*env)->ExceptionCheck(env)) {
    fprintf(stderr, "COOJA_JNI_ARRAY_ACQUIRE_FAILED op=write rel=0x%x len=%d\n",
            (unsigned int)rel_addr, (int)length);
    return;
  }
  memcpy((void *)absolute, mem, (size_t)length);
  (*env)->ReleaseByteArrayElements(env, mem_arr, mem, 0);
  if((*env)->ExceptionCheck(env)) {
    fprintf(stderr, "COOJA_JNI_WRITE_EXCEPTION rel=0x%x len=%d abs=0x%" PRIxPTR "\n",
            (unsigned int)rel_addr, (int)length, absolute);
  }
}
/*---------------------------------------------------------------------------*/
/**
 * \brief      Let mote execute one "block" of code (tick mote).
 * \param env  JNI Environment interface pointer
 * \param obj  unused
 *
 *             Let mote defined by the active contiki processes and current
 *             process memory execute some program code. This code must not block
 *             or else this function will never return. A typical contiki
 *             process will return when it executes PROCESS_WAIT..() statements.
 *
 *             Before the control is left to contiki processes, any messages
 *             from the Java part are handled. These may for example be
 *             incoming network data. After the contiki processes return control,
 *             messages to the Java part are also handled (those which may need
 *             special attention).
 *
 *             This is a JNI function and should only be called via the
 *             responsible Java part (MoteType.java).
 */
JNIEXPORT void JNICALL
Java_org_contikios_cooja_corecomm_CLASSNAME_tick(JNIEnv *env, jobject obj)
{
  simProcessRunValue = 0;

  /* Let all simulation interfaces act first */
  doActionsBeforeTick();

  /* Poll etimer process */
  if(etimer_pending()) {
    etimer_request_poll();
  }

  /* Let rtimers run.
   * Sets simProcessRunValue */
  cooja_mt_exec(&rtimer_thread);

  if(simProcessRunValue == 0) {
    /* Rtimers done: Let Contiki handle a few events.
     * Sets simProcessRunValue */
    cooja_mt_exec(&process_run_thread);
  }

  /* Let all simulation interfaces act before returning to java */
  doActionsAfterTick();

  /* Do we have any pending timers */
  simEtimerPending = etimer_pending();

  /* Save nearest expiration time */
  simEtimerNextExpirationTime = etimer_next_expiration_time();

}
/*---------------------------------------------------------------------------*/
/**
 * \brief      Set the relative memory address of the reference variable.
 * \param env  JNI Environment interface pointer
 * \param obj  unused
 * \param addr Relative memory address
 *
 *             This is a JNI function and should only be called via the
 *             responsible Java part (MoteType.java).
 */
JNIEXPORT void JNICALL
Java_org_contikios_cooja_corecomm_CLASSNAME_setReferenceAddress(JNIEnv *env, jobject obj, jint addr)
{
  uintptr_t runtime_address = (uintptr_t)&referenceVar;
  uintptr_t link_address;
  struct CoojaPhdrLookup lookup;
  if(addr < 0) {
    coojaThrow(env, "Cooja JNI referenceVar link address is negative");
    return;
  }
  link_address = (uintptr_t)(uint32_t)addr;
  if(runtime_address < link_address || runtime_address - link_address > (uintptr_t)LONG_MAX) {
    coojaThrow(env, "Cooja JNI referenceVar relocation is not representable as a positive long");
    return;
  }
  referenceVar = (long)(runtime_address - link_address);
  memset(&lookup, 0, sizeof(lookup));
  lookup.start = runtime_address;
  if(dl_iterate_phdr(coojaFindObject, &lookup) != 1) {
    fprintf(stderr, "COOJA_JNI_REFERENCE_INVALID runtime=0x%" PRIxPTR " link=0x%x\n",
            runtime_address, (unsigned int)addr);
    coojaThrow(env, "Cooja JNI could not identify the shared object containing referenceVar");
    return;
  }
  coojaObjectBase = lookup.module_base;
  fprintf(stderr, "COOJA_JNI_REFERENCE link=0x%x runtime=0x%" PRIxPTR " referenceVar=0x%lx object=%s PT_LOAD=[0x%" PRIxPTR ",0x%" PRIxPTR ") flags=%c%c%c\n",
          (unsigned int)addr, runtime_address, referenceVar,
          lookup.module_name == NULL ? "<unknown>" : lookup.module_name,
          lookup.segment_start, lookup.segment_end,
          (lookup.segment_flags & PF_R) ? 'R' : '-',
          (lookup.segment_flags & PF_W) ? 'W' : '-',
          (lookup.segment_flags & PF_X) ? 'X' : '-');
}

JNIEXPORT void JNICALL
Java_org_contikios_cooja_corecomm_CLASSNAME_setMemorySections(JNIEnv *env, jobject obj,
                                                               jint data_start, jint data_length,
                                                               jint bss_start, jint bss_length)
{
  uintptr_t data_end, bss_end;
  (void)obj;
  if(data_start < 0 || data_length <= 0 || bss_start < 0 || bss_length <= 0) {
    coojaThrow(env, "Cooja JNI received invalid data/BSS section bounds");
    return;
  }
  coojaDataStartRel = (uintptr_t)(uint32_t)data_start;
  coojaBssStartRel = (uintptr_t)(uint32_t)bss_start;
  if((uintptr_t)data_length > UINTPTR_MAX - coojaDataStartRel
      || (uintptr_t)bss_length > UINTPTR_MAX - coojaBssStartRel) {
    coojaThrow(env, "Cooja JNI data/BSS section end overflow");
    return;
  }
  data_end = coojaDataStartRel + (uintptr_t)data_length;
  bss_end = coojaBssStartRel + (uintptr_t)bss_length;
  if(coojaDataStartRel < bss_end && coojaBssStartRel < data_end) {
    coojaThrow(env, "Cooja JNI data/BSS section ranges overlap");
    return;
  }
  coojaDataEndRel = data_end;
  coojaBssEndRel = bss_end;
  coojaMemorySectionsConfigured = 1;
  fprintf(stderr, "COOJA_JNI_SECTIONS data=[0x%" PRIxPTR ",0x%" PRIxPTR ") bss=[0x%" PRIxPTR ",0x%" PRIxPTR ") objectBase=0x%" PRIxPTR "\n",
          coojaDataStartRel, coojaDataEndRel, coojaBssStartRel, coojaBssEndRel, coojaObjectBase);
}
