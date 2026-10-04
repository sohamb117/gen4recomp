/*
 * Every wasm import the runtime provides, with the exact C signatures
 * wasm2c gives them (i32 -> u32, i64 -> u64, first parameter the import
 * module's instance).
 *
 * Import symbols are named after the import module, not the guest module,
 * so one definition serves every game linked into the binary. The per-module
 * glue includes this header after the generated one: if a guest declares an
 * import with a different signature the glue fails to compile, instead of
 * the mismatch surfacing as a crash. Imports a guest does not use are simply
 * never referenced; an import the runtime lacks fails at link time.
 */
#ifndef NP_IMPORTS_H
#define NP_IMPORTS_H

#include <stdint.h>

struct w2c_np__host;
struct w2c_wasi__snapshot__preview1;

/* module "np_host": see np_guest_abi.h */
uint32_t w2c_np__host_fiber_self(struct w2c_np__host *h);
uint32_t w2c_np__host_fiber_create(struct w2c_np__host *h, uint32_t shadow_stack_top, uint32_t arg);
void w2c_np__host_fiber_switch(struct w2c_np__host *h, uint32_t to);
void w2c_np__host_fiber_destroy(struct w2c_np__host *h, uint32_t fiber);
void w2c_np__host_vblank(struct w2c_np__host *h, uint32_t desc);
uint32_t w2c_np__host_rom_size(struct w2c_np__host *h);
uint32_t w2c_np__host_rom_read(struct w2c_np__host *h, uint32_t offset, uint32_t dst, uint32_t len);
uint32_t w2c_np__host_save_load(struct w2c_np__host *h, uint32_t dst, uint32_t len);
uint32_t w2c_np__host_save_store(struct w2c_np__host *h, uint32_t src, uint32_t len);
uint64_t w2c_np__host_rtc_now(struct w2c_np__host *h);
void w2c_np__host_log(struct w2c_np__host *h, uint32_t text, uint32_t len);
void w2c_np__host_trap(struct w2c_np__host *h, uint32_t text, uint32_t len);

/* module "wasi_snapshot_preview1": the subset in np_wasi.c */
uint32_t w2c_wasi__snapshot__preview1_args_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t argv,
                                               uint32_t argv_buf);
uint32_t w2c_wasi__snapshot__preview1_args_sizes_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t argc_out,
                                                     uint32_t buf_size_out);
uint32_t w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t environ_ptrs,
                                                  uint32_t environ_buf);
uint32_t w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t count_out,
                                                        uint32_t buf_size_out);
uint32_t w2c_wasi__snapshot__preview1_clock_res_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t id,
                                                    uint32_t out);
uint32_t w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t id,
                                                     uint64_t precision, uint32_t out);
uint32_t w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd);
uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                    uint32_t out);
uint32_t w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                          uint32_t flags);
uint32_t w2c_wasi__snapshot__preview1_fd_readdir(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t buf,
                                                 uint32_t buf_len, uint64_t cookie, uint32_t bufused_out);
uint32_t w2c_wasi__snapshot__preview1_path_create_directory(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                            uint32_t path, uint32_t path_len);
uint32_t w2c_wasi__snapshot__preview1_path_filestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                        uint32_t flags, uint32_t path, uint32_t path_len,
                                                        uint32_t out);
uint32_t w2c_wasi__snapshot__preview1_fd_prestat_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                     uint32_t out);
uint32_t w2c_wasi__snapshot__preview1_fd_prestat_dir_name(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd,
                                                          uint32_t path, uint32_t path_len);
uint32_t w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs,
                                              uint32_t iovs_len, uint32_t nread_out);
uint32_t w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint64_t offset,
                                              uint32_t whence, uint32_t newoffset_out);
uint32_t w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w, uint32_t fd, uint32_t iovs,
                                               uint32_t iovs_len, uint32_t nwritten_out);
uint32_t w2c_wasi__snapshot__preview1_path_open(struct w2c_wasi__snapshot__preview1 *w, uint32_t dirfd,
                                                uint32_t dirflags, uint32_t path, uint32_t path_len, uint32_t oflags,
                                                uint64_t rights_base, uint64_t rights_inheriting, uint32_t fdflags,
                                                uint32_t fd_out);
void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w, uint32_t code);
uint32_t w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1 *w, uint32_t buf,
                                                 uint32_t len);
uint32_t w2c_wasi__snapshot__preview1_sched_yield(struct w2c_wasi__snapshot__preview1 *w);

#endif /* NP_IMPORTS_H */
