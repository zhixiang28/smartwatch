/* decoder state (~60 KB with SBR) goes to PSRAM */
#include <string.h>
#include "esp_heap_caps.h"
#include "utils/helix_memory.h"
void *helix_malloc(int size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}
void helix_free(void *ptr) { free(ptr); }
