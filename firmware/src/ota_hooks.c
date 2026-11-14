#include "../include/ceiling3_sdk.h"

/*
 * Equivalent in C of patch #3 (0x100f831c): allow both local UDP (otu, bit 0)
 * and cloud TLS (ots, bit 1) requests to trigger miIO.ota.
 */
__attribute__((used))
int ceiling3_is_ota_rpc_allowed(const miio_rpc_delegate_arg_t *req)
{
    if (req == NULL || req->pload == NULL || req->pload_len == 0u) {
        return 0;
    }
    return (req->channel_flags & 0x03u) != 0u ? 1 : 0;
}

/* Validate staging partition bounds and run firmware CRC32 check before commit. */
__attribute__((used))
int ceiling3_verify_ota_image(const ota_app_ctx_t *ctx)
{
    if (ctx == NULL) {
        return -1;
    }
    if (ctx->total_size < 0x2800u || ctx->total_size > ctx->flash_part.partition_len) {
        return -2;
    }
    return arch_ota_check_crc(&ctx->flash_part, ctx->total_size);
}
