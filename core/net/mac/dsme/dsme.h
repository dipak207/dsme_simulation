#ifndef __DSME_H__
#define __DSME_H__

#include "contiki.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const struct mac_driver dsmemac_driver;

uint32_t dsmeAssociated();
/* Request an uplink DSME GTS to the peer's short address. */
uint8_t dsmeRequestGTS(uint16_t peerShortAddress, uint8_t numSlots);
uint8_t dsmeHasValidGTS(uint16_t peerShortAddress);
/* Emit the current MO protocol state as one machine-readable log record. */
void dsmeLogMOProtocolSnapshot(void);

#ifdef __cplusplus
}
#endif
  
#endif /* __DSMEMAC_H__ */
