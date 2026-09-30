#ifndef LA64M68_INPUT_H
#define LA64M68_INPUT_H

#include "../core/vhid.h"

/* Host input backend for vHID. Linux: evdev (/dev/input/event*) with
 * grab/ungrab. null until evdev lands; BSD via sysmouse/wscons later. */

typedef struct la64m68_input la64m68_input;

/* devices_csv: comma-separated /dev/input/eventN paths; NULL = autoscan */
la64m68_input *la64m68_input_create(la64m68_vhid *vhid,
                                    const char *devices_csv);
/* pump pending host events into vhid queue (call per frame/tick) */
void           la64m68_input_poll(la64m68_input *i);
void           la64m68_input_grab(la64m68_input *i, int grab);
void           la64m68_input_destroy(la64m68_input *i);

#endif
