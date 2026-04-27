/* vaporware/include/vape.h — Framework-internal hardware safety stubs
 *
 * These functions are called automatically by the app framework (app.c).
 * Apps do not need to call them directly.
 */
#ifndef VAPE_H
#define VAPE_H

#include <stdint.h>

/* Called by the framework before clock_init(). */
void vape_safety_init(void);

/* Called by the framework after display_init(). */
void vape_init(void);

#endif /* VAPE_H */
