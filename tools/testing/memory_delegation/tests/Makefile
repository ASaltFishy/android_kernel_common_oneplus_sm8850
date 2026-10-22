# SPDX-License-Identifier: GPL-2.0
#
# Build aarch64 static test binaries for QEMU initramfs.
#
# Outputs:
#   out/scudo_shared_arena_test
#   out/scudo_shared_arena_latency_bench
#   out/shared_arena_image_pipeline_app
#   out/shared_arena_cache_savings_app
#   out/shared_arena_sqlite_app
#   out/shared_arena_zstd_app
#   out/memory_delegation_broker
#   out/md_mem_pressure_app
#   out/md_camera_arena_donor
#   out/md_alloc_trace_replay
#   out/libscudo_md_android.so
#   out/libscudo_md_preload.so
#

OUT_DIR := $(CURDIR)/out
SRC_DIR := $(CURDIR)/src

# memory_delegation/tests -> memory_delegation -> testing -> tools -> common -> kernel_platform
KERNEL_PLATFORM_DIR := $(abspath $(CURDIR)/../../../../..)
SCUDO_DIR := $(KERNEL_PLATFORM_DIR)/../../llvm-20/compiler-rt/lib/scudo/standalone
ZSTD_ROOT ?= /tmp/md-zstd-src/zstd-1.5.6
ZSTD_DIR := $(ZSTD_ROOT)/lib

AARCH64_CC  ?= aarch64-linux-gnu-gcc
AARCH64_CXX ?= aarch64-linux-gnu-g++

MODE ?= linux
ANDROID_NDK ?= /home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26
ANDROID_API ?= 34
ANDROID_CC  ?= $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(ANDROID_API)-clang
ANDROID_CXX ?= $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(ANDROID_API)-clang++
SCUDO_PRELOAD_CPP_WRAPPERS ?= 0
SCUDO_SHARED_ARENA_PROFILE_BUILD ?= 0
SCUDO_PROFILE_DEFS :=
ifeq ($(SCUDO_SHARED_ARENA_PROFILE_BUILD),1)
SCUDO_PROFILE_DEFS += -DSCUDO_SHARED_ARENA_ENABLE_PROFILE=1
endif

ifeq ($(MODE),android)
CC := $(ANDROID_CC)
CXX := $(ANDROID_CXX)
CFLAGS_COMMON   := -O2 -g -fPIE -pie -pthread -Wall -Wextra -Werror -fno-emulated-tls
SCUDO_PLATFORM_DEFS :=
LDFLAGS_EXTRA := -static-libstdc++ -ldl -latomic -llog -landroid
else
CC := $(AARCH64_CC)
CXX := $(AARCH64_CXX)
CFLAGS_COMMON   := -O2 -g -static -pthread -Wall -Wextra -Werror
SCUDO_PLATFORM_DEFS := -DSCUDO_LINUX=1 -DSCUDO_ANDROID=0
LDFLAGS_EXTRA :=
endif

CXXFLAGS_COMMON := $(CFLAGS_COMMON) -std=c++17 -fno-exceptions -fno-rtti

.PHONY: all clean scudo_shared_arena_test scudo_shared_arena_latency_bench shared_arena_image_pipeline_app shared_arena_cache_savings_app shared_arena_sqlite_app shared_arena_zstd_app sched_yield_latency_bench memory_delegation_broker md_mem_pressure_app md_camera_arena_donor md_alloc_trace_replay libscudo_md_android_so libscudo_md_preload_so print-vars

all: scudo_shared_arena_test scudo_shared_arena_latency_bench shared_arena_image_pipeline_app shared_arena_cache_savings_app shared_arena_sqlite_app shared_arena_zstd_app sched_yield_latency_bench memory_delegation_broker md_mem_pressure_app md_camera_arena_donor

print-vars:
	@echo "OUT_DIR=$(OUT_DIR)"
	@echo "KERNEL_PLATFORM_DIR=$(KERNEL_PLATFORM_DIR)"
	@echo "SCUDO_DIR=$(SCUDO_DIR)"
	@echo "ZSTD_DIR=$(ZSTD_DIR)"
	@echo "ZSTD_ROOT=$(ZSTD_ROOT)"
	@echo "AARCH64_CC=$(AARCH64_CC)"
	@echo "AARCH64_CXX=$(AARCH64_CXX)"
	@echo "MODE=$(MODE)"
	@echo "ANDROID_NDK=$(ANDROID_NDK)"
	@echo "ANDROID_CC=$(ANDROID_CC)"
	@echo "ANDROID_CXX=$(ANDROID_CXX)"

$(OUT_DIR):
	@mkdir -p "$(OUT_DIR)"


scudo_shared_arena_test: $(OUT_DIR)/scudo_shared_arena_test
scudo_shared_arena_latency_bench: $(OUT_DIR)/scudo_shared_arena_latency_bench
shared_arena_image_pipeline_app: $(OUT_DIR)/shared_arena_image_pipeline_app
shared_arena_cache_savings_app: $(OUT_DIR)/shared_arena_cache_savings_app
shared_arena_sqlite_app: $(OUT_DIR)/shared_arena_sqlite_app
shared_arena_zstd_app: $(OUT_DIR)/shared_arena_zstd_app
sched_yield_latency_bench: $(OUT_DIR)/sched_yield_latency_bench
memory_delegation_broker: $(OUT_DIR)/memory_delegation_broker
md_mem_pressure_app: $(OUT_DIR)/md_mem_pressure_app
md_camera_arena_donor: $(OUT_DIR)/md_camera_arena_donor
md_alloc_trace_replay: $(OUT_DIR)/md_alloc_trace_replay
libscudo_md_android_so: $(OUT_DIR)/libscudo_md_android.so
libscudo_md_preload_so: $(OUT_DIR)/libscudo_md_preload.so

# Scudo standalone allocator + C wrappers.  Keep this source list aligned with
# standalone/CMakeLists.txt so malloc/free in the smoke binary are Scudo's
# wrappers, not glibc malloc/free.
SCUDO_SRC := \
	$(SCUDO_DIR)/alloc_trace.cpp \
	$(SCUDO_DIR)/checksum.cpp \
	$(SCUDO_DIR)/common.cpp \
	$(SCUDO_DIR)/condition_variable_linux.cpp \
	$(SCUDO_DIR)/crc32_hw.cpp \
	$(SCUDO_DIR)/flags_parser.cpp \
	$(SCUDO_DIR)/flags.cpp \
	$(SCUDO_DIR)/fuchsia.cpp \
	$(SCUDO_DIR)/linux.cpp \
	$(SCUDO_DIR)/mem_map.cpp \
	$(SCUDO_DIR)/mem_map_fuchsia.cpp \
	$(SCUDO_DIR)/mem_map_linux.cpp \
	$(SCUDO_DIR)/release.cpp \
	$(SCUDO_DIR)/report.cpp \
	$(SCUDO_DIR)/report_linux.cpp \
	$(SCUDO_DIR)/shared_arena_linux.cpp \
	$(SCUDO_DIR)/string_utils.cpp \
	$(SCUDO_DIR)/timing.cpp \
	$(SCUDO_DIR)/wrappers_c.cpp

SCUDO_BENCH_SRC := \
	$(SCUDO_DIR)/alloc_trace.cpp \
	$(SCUDO_DIR)/checksum.cpp \
	$(SCUDO_DIR)/common.cpp \
	$(SCUDO_DIR)/condition_variable_linux.cpp \
	$(SCUDO_DIR)/crc32_hw.cpp \
	$(SCUDO_DIR)/flags_parser.cpp \
	$(SCUDO_DIR)/flags.cpp \
	$(SCUDO_DIR)/fuchsia.cpp \
	$(SCUDO_DIR)/linux.cpp \
	$(SCUDO_DIR)/mem_map.cpp \
	$(SCUDO_DIR)/mem_map_fuchsia.cpp \
	$(SCUDO_DIR)/mem_map_linux.cpp \
	$(SCUDO_DIR)/release.cpp \
	$(SCUDO_DIR)/report.cpp \
	$(SCUDO_DIR)/report_linux.cpp \
	$(SCUDO_DIR)/shared_arena_linux.cpp \
	$(SCUDO_DIR)/string_utils.cpp \
	$(SCUDO_DIR)/timing.cpp

SCUDO_PRELOAD_SRC := \
	$(SCUDO_SRC) \
	$(SRC_DIR)/bionic_malloc_debug_scudo_adapter.cpp \
	$(if $(filter 1,$(SCUDO_PRELOAD_CPP_WRAPPERS)),$(SCUDO_DIR)/wrappers_cpp.cpp)

ZSTD_SRC := \
	$(ZSTD_DIR)/common/debug.c \
	$(ZSTD_DIR)/common/entropy_common.c \
	$(ZSTD_DIR)/common/error_private.c \
	$(ZSTD_DIR)/common/fse_decompress.c \
	$(ZSTD_DIR)/common/pool.c \
	$(ZSTD_DIR)/common/threading.c \
	$(ZSTD_DIR)/common/xxhash.c \
	$(ZSTD_DIR)/common/zstd_common.c \
	$(ZSTD_DIR)/compress/fse_compress.c \
	$(ZSTD_DIR)/compress/hist.c \
	$(ZSTD_DIR)/compress/huf_compress.c \
	$(ZSTD_DIR)/compress/zstd_compress.c \
	$(ZSTD_DIR)/compress/zstd_compress_literals.c \
	$(ZSTD_DIR)/compress/zstd_compress_sequences.c \
	$(ZSTD_DIR)/compress/zstd_compress_superblock.c \
	$(ZSTD_DIR)/compress/zstd_double_fast.c \
	$(ZSTD_DIR)/compress/zstd_fast.c \
	$(ZSTD_DIR)/compress/zstd_lazy.c \
	$(ZSTD_DIR)/compress/zstd_ldm.c \
	$(ZSTD_DIR)/compress/zstd_opt.c \
	$(ZSTD_DIR)/decompress/huf_decompress.c \
	$(ZSTD_DIR)/decompress/zstd_ddict.c \
	$(ZSTD_DIR)/decompress/zstd_decompress.c \
	$(ZSTD_DIR)/decompress/zstd_decompress_block.c

$(OUT_DIR)/scudo_shared_arena_test: \
		$(SRC_DIR)/scudo_shared_arena_test.cpp \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	# Scudo sources intentionally ignore some warn_unused_result calls; don't
	# fail the build for that in this standalone smoke test.
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SRC_DIR)/scudo_shared_arena_test.cpp" \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/scudo_shared_arena_latency_bench: \
		$(SCUDO_DIR)/tests/shared_arena_latency_bench.cpp \
		$(SCUDO_BENCH_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result -Wno-error=deprecated \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SCUDO_DIR)/tests/shared_arena_latency_bench.cpp" \
		$(SCUDO_BENCH_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/libscudo_md_android.so: \
		$(SCUDO_PRELOAD_SRC) \
		$(SRC_DIR)/bionic_malloc_debug_scudo.map \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result -Wno-error \
		-shared -fPIC \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-DSCUDO_FORCE_STRONG_MALLOC_WRAPPERS \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		$(SCUDO_PRELOAD_SRC) \
		-Wl,-Bsymbolic-functions \
		-Wl,-soname,libc_malloc_debug.so \
		-Wl,--version-script,$(SRC_DIR)/bionic_malloc_debug_scudo.map \
		$(filter-out -llog -landroid,$(LDFLAGS_EXTRA))
	@file "$@"

$(OUT_DIR)/libscudo_md_preload.so: \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result -Wno-error \
		-shared -fPIC \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-DSCUDO_FORCE_STRONG_MALLOC_WRAPPERS \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		$(SCUDO_SRC) \
		-Wl,-Bsymbolic-functions \
		$(filter-out -llog -landroid,$(LDFLAGS_EXTRA))
	@file "$@"

$(OUT_DIR)/shared_arena_image_pipeline_app: \
		$(SRC_DIR)/shared_arena_image_pipeline_app.cpp \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SRC_DIR)/shared_arena_image_pipeline_app.cpp" \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/shared_arena_cache_savings_app: \
		$(SRC_DIR)/shared_arena_cache_savings_app.cpp \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SRC_DIR)/shared_arena_cache_savings_app.cpp" \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/shared_arena_sqlite_app: \
		$(SRC_DIR)/shared_arena_sqlite_app.cpp \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SRC_DIR)/shared_arena_sqlite_app.cpp" \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/shared_arena_zstd_app: \
		$(SRC_DIR)/shared_arena_zstd_app.cpp \
		$(OUT_DIR)/zstd-lib.o \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result -Wno-error=deprecated \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-DZSTD_LEGACY_SUPPORT=0 \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-I"$(ZSTD_DIR)" \
		-I"$(ZSTD_DIR)/common" \
		-o "$@" \
		"$(SRC_DIR)/shared_arena_zstd_app.cpp" \
		$(SCUDO_SRC) \
		"$(OUT_DIR)/zstd-lib.o" \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/zstd-lib.o: \
		$(ZSTD_SRC) \
		| $(OUT_DIR)
	$(CC) $(CFLAGS_COMMON) -Wno-unused-result \
		-DZSTD_LEGACY_SUPPORT=0 \
		-DZSTD_DISABLE_ASM=1 \
		-I"$(ZSTD_DIR)" \
		-I"$(ZSTD_DIR)/common" \
		-r $(ZSTD_SRC) \
		-o "$@"

$(OUT_DIR)/sched_yield_latency_bench: \
		$(SRC_DIR)/sched_yield_latency_bench.c \
		| $(OUT_DIR)
	$(CC) $(CFLAGS_COMMON) \
		-o "$@" \
		"$(SRC_DIR)/sched_yield_latency_bench.c" \
		$(if $(filter android,$(MODE)),-latomic)
	@file "$@"

$(OUT_DIR)/memory_delegation_broker: \
		$(SRC_DIR)/memory_delegation_broker.cpp \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) \
		-o "$@" \
		"$(SRC_DIR)/memory_delegation_broker.cpp" \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/md_mem_pressure_app: \
		$(SRC_DIR)/md_mem_pressure_app.c \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		-x c++ \
		"$(SRC_DIR)/md_mem_pressure_app.c" \
		-x none \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/md_camera_arena_donor: \
		$(SRC_DIR)/md_camera_arena_donor.cpp \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SRC_DIR)/md_camera_arena_donor.cpp" \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

$(OUT_DIR)/md_alloc_trace_replay: \
		$(SRC_DIR)/md_alloc_trace_replay.cpp \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		$(SCUDO_PROFILE_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SRC_DIR)/md_alloc_trace_replay.cpp" \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

clean:
	@rm -rf "$(OUT_DIR)"

# ABI v2 kernel protocol test, independent of the old Scudo ring producer.
.PHONY: shared_log_v2_test
shared_log_v2_test: $(OUT_DIR)/shared_log_v2_test
$(OUT_DIR)/shared_log_v2_test: shared_log_v2_test.c shared_log_v2.h ../../../../include/uapi/linux/memory_delegation.h | $(OUT_DIR)
	$(CC) $(CFLAGS_COMMON) -std=gnu11 $< -o $@
