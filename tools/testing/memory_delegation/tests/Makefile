# SPDX-License-Identifier: GPL-2.0
#
# Build aarch64 static test binaries for QEMU initramfs.
#
# Outputs:
#   out/scudo_shared_arena_test
#

OUT_DIR := $(CURDIR)/out
SRC_DIR := $(CURDIR)/src

# memory_delegation/tests -> memory_delegation -> testing -> tools -> common -> kernel_platform
KERNEL_PLATFORM_DIR := $(abspath $(CURDIR)/../../../../..)
SCUDO_DIR := $(KERNEL_PLATFORM_DIR)/../../llvm-20/compiler-rt/lib/scudo/standalone

AARCH64_CC  ?= aarch64-linux-gnu-gcc
AARCH64_CXX ?= aarch64-linux-gnu-g++

MODE ?= linux
ANDROID_NDK ?= /home/lrc/patent/kernel/kernel_platform/prebuilts/ndk-r26
ANDROID_API ?= 34
ANDROID_CXX ?= $(ANDROID_NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$(ANDROID_API)-clang++

ifeq ($(MODE),android)
CXX := $(ANDROID_CXX)
CFLAGS_COMMON   := -O2 -g -fPIE -pie -pthread -Wall -Wextra -Werror -fno-emulated-tls
SCUDO_PLATFORM_DEFS :=
LDFLAGS_EXTRA := -static-libstdc++ -ldl -latomic -llog -landroid
else
CXX := $(AARCH64_CXX)
CFLAGS_COMMON   := -O2 -g -static -pthread -Wall -Wextra -Werror
SCUDO_PLATFORM_DEFS := -DSCUDO_LINUX=1 -DSCUDO_ANDROID=0
LDFLAGS_EXTRA :=
endif

CXXFLAGS_COMMON := $(CFLAGS_COMMON) -fno-exceptions -fno-rtti

.PHONY: all clean scudo_shared_arena_test print-vars

all: scudo_shared_arena_test

print-vars:
	@echo "OUT_DIR=$(OUT_DIR)"
	@echo "KERNEL_PLATFORM_DIR=$(KERNEL_PLATFORM_DIR)"
	@echo "SCUDO_DIR=$(SCUDO_DIR)"
	@echo "AARCH64_CC=$(AARCH64_CC)"
	@echo "AARCH64_CXX=$(AARCH64_CXX)"
	@echo "MODE=$(MODE)"
	@echo "ANDROID_NDK=$(ANDROID_NDK)"
	@echo "ANDROID_CXX=$(ANDROID_CXX)"

$(OUT_DIR):
	@mkdir -p "$(OUT_DIR)"


scudo_shared_arena_test: $(OUT_DIR)/scudo_shared_arena_test

# Minimal Scudo source set needed by SharedArena on Linux.
SCUDO_SRC := \
	$(SCUDO_DIR)/common.cpp \
	$(SCUDO_DIR)/string_utils.cpp \
	$(SCUDO_DIR)/linux.cpp \
	$(SCUDO_DIR)/mem_map_linux.cpp \
	$(SCUDO_DIR)/report.cpp \
	$(SCUDO_DIR)/report_linux.cpp \
	$(SCUDO_DIR)/shared_arena_linux.cpp

$(OUT_DIR)/scudo_shared_arena_test: \
		$(SRC_DIR)/scudo_shared_arena_test.cpp \
		$(SCUDO_SRC) \
		| $(OUT_DIR)
	# Scudo sources intentionally ignore some warn_unused_result calls; don't
	# fail the build for that in this standalone smoke test.
	$(CXX) $(CXXFLAGS_COMMON) -Wno-unused-result \
		$(SCUDO_PLATFORM_DEFS) \
		-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL \
		-I"$(SCUDO_DIR)" \
		-I"$(SCUDO_DIR)/include" \
		-o "$@" \
		"$(SRC_DIR)/scudo_shared_arena_test.cpp" \
		$(SCUDO_SRC) \
		$(LDFLAGS_EXTRA)
	@file "$@"

clean:
	@rm -rf "$(OUT_DIR)"
