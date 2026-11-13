# Build Arbiter's QNX binaries with the SDP 8.0 toolchain.
#
#   source ~/qnx800/qnxsdp-env.sh
#   make -C qnx ARCH=x86_64        # QEMU
#   make -C qnx ARCH=aarch64le     # Raspberry Pi 5
#
# Output goes to build/$(ARCH)/ at the repo root (ignored by git).
#
# qcc -V selects the compiler and target:
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/q/qcc.html
# _QNX_SOURCE exposes QNX extensions (e.g. sigval.sival_long) under -std=c11;
# see <sys/platform.h> in the 8.0 SDP.

ARCH ?= x86_64

QCC_VARIANT_x86_64    := gcc_ntox86_64
QCC_VARIANT_aarch64le := gcc_ntoaarch64le
QCC_VARIANT := $(QCC_VARIANT_$(ARCH))
ifeq ($(QCC_VARIANT),)
$(error ARCH must be x86_64 or aarch64le, got '$(ARCH)')
endif
ifeq ($(QNX_TARGET),)
$(error QNX_TARGET is not set: run 'source ~/qnx800/qnxsdp-env.sh' first)
endif

CC     := qcc -V$(QCC_VARIANT)
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -D_QNX_SOURCE -Icommon
OUT    := ../build/$(ARCH)

BINS := $(OUT)/hello $(OUT)/msgpass

.PHONY: all clean
all: $(BINS)

$(OUT):
	mkdir -p $@

$(OUT)/hello: samples/hello/hello.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/msgpass: samples/msgpass/msgpass.c common/arb_pulses.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT)
