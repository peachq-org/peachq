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
 * retained graph ever references another ipc symbol, add its stub here. */
#include "core/ipc.h"
#include <rayforce.h>

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

size_t ray_ipc_decompress(const uint8_t* src, size_t clen, uint8_t* dst,
                          size_t dst_len) {
    (void)src; (void)clen; (void)dst; (void)dst_len;
    return 0;
}
