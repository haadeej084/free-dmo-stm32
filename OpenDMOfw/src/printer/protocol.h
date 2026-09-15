/* OpenDMOfw - open print protocol (see PROTOCOL.md for the format). */
#ifndef OP57_PROTOCOL_H
#define OP57_PROTOCOL_H
#include <stdint.h>

void protocol_init(void);
void protocol_reset(void);
/* Feed Bulk-OUT data (from the USB IRQ). Copies into the ring buffer. */
void protocol_feed(const uint8_t *data, uint16_t len);
/* In the main loop: processes buffered bytes, drives head/motor, reopens USB RX. */
void protocol_task(void);

#endif
