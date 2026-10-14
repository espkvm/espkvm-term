/* esp-hosted 3.0.9 takes every SDIO packet buffer from eh_host_port_dma_alloc(),
 * which ignores CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM and always uses internal
 * DMA RAM (~25 KB free here). While video flows that runs dry, packets drop
 * ("eh_sdio: mempool OOM (RX)") and TCP stalls. Take them from PSRAM instead;
 * linked with --wrap, see CMakeLists.txt. */

#include <stddef.h>
#include "esp_heap_caps.h"

#define LINE 64 /* P4 cache line: PSRAM DMA buffers must be aligned and padded */

void *__wrap_eh_host_port_dma_alloc(size_t n)
{
    size_t len = (n + LINE - 1) & ~(size_t)(LINE - 1);
    void *p = heap_caps_aligned_alloc(LINE, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_DMA);
}
