/* ipc_stub — inert IPC/networking symbols for the WebAssembly build.
 *
 * src/core/ipc.c is the TCP server/client (select/fd_set/struct timeval) and
 * does not compile under emscripten's wasm sysroot, so Makefile.wasm drops it
 * from the source set. A handful of TUs that ARE retained (syscmd.c, system.c,
 * repl.c, store/journal.c) reference a few ipc symbols along code paths the
 * browser q REPL never takes — there is no networking in a browser tab. These
 * stubs satisfy the linker and fail inertly (a handle open never succeeds, a
 * send returns an IPC error) so the reachable q eval/format path is unaffected.
 *
 * Kept in wasm/ so the native tree and the frozen base stay untouched. If the
 * retained graph ever references another ipc symbol, add its stub here.
 *
 * EXCEPTION: ray_ipc_decompress is NOT stubbed. Despite living in ipc.c it is a
 * pure RLE+delta decompressor with no networking, and src/store/journal.c calls
 * it to read compressed journal frames (.log.replay / .log.validate). Stubbing
 * it would silently treat every compressed journal as corrupt, so the real
 * implementation is preserved here verbatim from src/core/ipc.c. */
#include "core/ipc.h"
#include "mem/sys.h"     /* ray_sys_alloc / ray_sys_free (real decompress) */
#include <rayforce.h>
#include <string.h>      /* memset / memcpy (real decompress) */

int64_t ray_ipc_current_handle(void) { return -1; }

int64_t ray_ipc_listen(ray_poll_t* poll, uint16_t port) {
    (void)poll; (void)port;
    return -1;
}

int64_t ray_ipc_connect(const char* host, uint16_t port, const char* user,
                        const char* password, int timeout_ms) {
    (void)host; (void)port; (void)user; (void)password; (void)timeout_ms;
    return -1; /* refused: no networking in wasm */
}

void ray_ipc_close(int64_t handle) { (void)handle; }

ray_t* ray_ipc_send(int64_t handle, ray_t* msg) {
    (void)handle; (void)msg;
    return ray_error("ipc", "no networking in wasm"); /* inert */
}

ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg) {
    (void)handle; (void)msg;
    return RAY_ERR_IO;
}

/* Real RLE+delta decompressor, preserved verbatim from src/core/ipc.c so
 * compressed journal replay/validate works under WASM. Pure; no networking. */
size_t ray_ipc_decompress(const uint8_t* src, size_t clen, uint8_t* dst,
                          size_t dst_len) {
    uint8_t* decoded = (uint8_t*)ray_sys_alloc(dst_len);
    if (!decoded) return 0;

    size_t si = 0;
    size_t di = 0;

    while (si < clen && di < dst_len) {
        int8_t count = (int8_t)src[si++];
        if (count > 0) {
            if (si >= clen) { ray_sys_free(decoded); return 0; }
            uint8_t val = src[si++];
            size_t n = (size_t)count;
            if (di + n > dst_len) { ray_sys_free(decoded); return 0; }
            memset(decoded + di, val, n);
            di += n;
        } else {
            size_t n = (size_t)(-(int)count);
            if (si + n > clen || di + n > dst_len) {
                ray_sys_free(decoded);
                return 0;
            }
            memcpy(decoded + di, src + si, n);
            si += n;
            di += n;
        }
    }

    /* Un-delta */
    if (di == 0) { ray_sys_free(decoded); return 0; }
    dst[0] = decoded[0];
    for (size_t i = 1; i < di; i++)
        dst[i] = (uint8_t)(decoded[i] + dst[i - 1]);

    ray_sys_free(decoded);
    return di;
}
