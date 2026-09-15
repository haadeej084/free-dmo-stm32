/* OpenDMOfw - persistent printer config (I2C EEPROM, with defaults). */
#ifndef OP57_STORE_H
#define OP57_STORE_H
#include <stdint.h>

#define OP_SKU_MAX 24

/* Config flags (op_config_t.flags). */
#define OP_FLAG_PAPER_FORCE (1u<<0)  /* always report paper present to the host (default) */

typedef struct {
    uint32_t magic;                 /* validity marker */
    char     sku[OP_SKU_MAX];       /* roll SKU (free config, no tag) */
    uint16_t label_count;           /* reported remaining count */
    uint8_t  density;               /* 1..16 base black level */
    uint8_t  flags;                 /* OP_FLAG_* bits */
} op_config_t;

void              store_init(void); /* I2C init + load (or defaults) */
const op_config_t*store_get(void);
op_config_t*      store_get_mut(void);
int               store_save(void); /* to EEPROM; 0 = ok, <0 = none/again */
void              store_load(void);
int               store_selftest(void); /* write+read a scratch area; 1 = match */

#endif
