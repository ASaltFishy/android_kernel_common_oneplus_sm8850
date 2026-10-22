// SPDX-License-Identifier: GPL-2.0
//
// Minimal bionic malloc-debug shim that redirects Android's debug malloc
// dispatch entry points to the Scudo allocator linked in this shared object.
//
// Android libc loads libc_malloc_debug.so when libc.debug.malloc.options is set
// and resolves the debug_* symbols below with dlsym().  We intentionally ignore
// malloc-debug options here: the library is used only as a controlled systemless
// entry point for the memory-delegation Scudo experiment.

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" void malloc_disable();
extern "C" void malloc_enable();
extern "C" int malloc_iterate(uintptr_t base, size_t size,
                              void (*callback)(uintptr_t, size_t, void *),
                              void *arg);

static bool option_contains(const char *options, const char *needle) {
  return options && needle && strstr(options, needle) != nullptr;
}

static void setenv_if_missing(const char *name, const char *value) {
  if (!getenv(name))
    setenv(name, value, 0);
}

extern "C" bool debug_initialize(const void *malloc_dispatch,
                                  bool *malloc_zygote_child,
                                  const char *options) {
  (void)malloc_dispatch;
  (void)malloc_zygote_child;
  if (option_contains(options, "md_shared")) {
    setenv_if_missing("SCUDO_SHARED_ARENA_EAGER_INIT", "1");
    setenv_if_missing("SCUDO_SHARED_ARENA_FORCE", "1");
    setenv_if_missing("SCUDO_SHARED_ARENA_DISABLE_FREELIST", "1");
    setenv_if_missing("SCUDO_SHARED_ARENA_DISABLE_FILL_ALLOC", "1");
    setenv_if_missing("SCUDO_SHARED_ARENA_TRACE_FILE",
                      "/data/local/tmp/md_system_scudo_trace.log");
  }
  if (option_contains(options, "md_trace"))
    setenv_if_missing("SCUDO_SHARED_ARENA_TRACE", "1");
  return true;
}

extern "C" void debug_finalize() {}

extern "C" void debug_dump_heap(const char *file_name) {
  (void)file_name;
}

extern "C" void debug_get_malloc_leak_info(uint8_t **info,
                                            size_t *overall_size,
                                            size_t *info_size,
                                            size_t *total_memory,
                                            size_t *backtrace_size) {
  if (info)
    *info = nullptr;
  if (overall_size)
    *overall_size = 0;
  if (info_size)
    *info_size = 0;
  if (total_memory)
    *total_memory = 0;
  if (backtrace_size)
    *backtrace_size = 0;
}

extern "C" bool debug_write_malloc_leak_info(FILE *fp) {
  (void)fp;
  return false;
}

extern "C" ssize_t debug_malloc_backtrace(void *pointer, uintptr_t *frames,
                                           size_t frame_count) {
  (void)pointer;
  (void)frames;
  (void)frame_count;
  return 0;
}

extern "C" void debug_free_malloc_leak_info(uint8_t *info) {
  free(info);
}

extern "C" size_t debug_malloc_usable_size(void *pointer) {
  return malloc_usable_size(pointer);
}

extern "C" void *debug_malloc(size_t size) {
  return malloc(size);
}

extern "C" void debug_free(void *pointer) {
  free(pointer);
}

extern "C" void *debug_aligned_alloc(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}

extern "C" void *debug_memalign(size_t alignment, size_t bytes) {
  return memalign(alignment, bytes);
}

extern "C" void *debug_realloc(void *pointer, size_t bytes) {
  return realloc(pointer, bytes);
}

extern "C" void *debug_calloc(size_t nmemb, size_t bytes) {
  return calloc(nmemb, bytes);
}

extern "C" struct mallinfo debug_mallinfo() {
  return mallinfo();
}

extern "C" int debug_malloc_info(int options, FILE *stream) {
  return malloc_info(options, stream);
}

extern "C" int debug_mallopt(int param, int value) {
  return mallopt(param, value);
}

extern "C" int debug_posix_memalign(void **memptr, size_t alignment,
                                     size_t size) {
  int ret = posix_memalign(memptr, alignment, size);
  return ret;
}

extern "C" int debug_malloc_iterate(uintptr_t base, size_t size,
                                    void (*callback)(uintptr_t, size_t,
                                                     void *),
                                    void *arg) {
  return malloc_iterate(base, size, callback, arg);
}

extern "C" int debug_iterate(uintptr_t base, size_t size,
                             void (*callback)(uintptr_t, size_t, void *),
                             void *arg) {
  return malloc_iterate(base, size, callback, arg);
}

extern "C" void debug_malloc_disable() {
  malloc_disable();
}

extern "C" void debug_malloc_enable() {
  malloc_enable();
}
