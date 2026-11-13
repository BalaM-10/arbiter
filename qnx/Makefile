# Build Arbiter's QNX binaries with the SDP 8.0 toolchain, and the host-side
# checks of the pure library.
#
#   source ~/qnx800/qnxsdp-env.sh
#   make -C qnx ARCH=x86_64        # QEMU
#   make -C qnx ARCH=aarch64le     # Raspberry Pi 5
#   make -C qnx host-test          # libcontract unit tests on the host, with sanitizers
#   make -C qnx analyze            # gcc -fanalyzer and cppcheck over libcontract
#   make -C qnx check-pure         # libcontract calls nothing outside the allowed C library set
#
# Output goes to build/$(ARCH)/ and build/host/ at the repo root (ignored by git).
#
# qcc -V selects the compiler and target:
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/q/qcc.html
# _QNX_SOURCE exposes QNX extensions (e.g. sigval.sival_long) under -std=c11;
# see <sys/platform.h> in the 8.0 SDP.

ARCH ?= x86_64

QCC_VARIANT_x86_64    := gcc_ntox86_64
QCC_VARIANT_aarch64le := gcc_ntoaarch64le
QCC_VARIANT := $(QCC_VARIANT_$(ARCH))
QNX_NM_x86_64    := ntox86_64-nm
QNX_NM_aarch64le := ntoaarch64-nm

CC       := qcc -V$(QCC_VARIANT)
INCLUDES := -Icommon -Ilibcontract -Itests
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -D_QNX_SOURCE $(INCLUDES)
LDLIBS   := -lm
OUT      := ../build/$(ARCH)

HOST_CC     ?= gcc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -O1 -g \
               -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all $(INCLUDES)
HOST_OUT    := ../build/host

LIBCONTRACT_SRC := libcontract/arb_contract.c
LIBCONTRACT_HDR := libcontract/arb_contract.h common/arb_contract_table.h common/arb_proto.h common/arb_arm_model.h

BINS := $(OUT)/hello $(OUT)/msgpass $(OUT)/test_libcontract

.PHONY: all clean qnx-env host-test analyze check-pure
all: qnx-env $(BINS)

qnx-env:
ifeq ($(QCC_VARIANT),)
	$(error ARCH must be x86_64 or aarch64le, got '$(ARCH)')
endif
	@test -n "$(QNX_TARGET)" || { echo "QNX_TARGET is not set: run 'source ~/qnx800/qnxsdp-env.sh' first" >&2; exit 1; }

$(OUT) $(HOST_OUT):
	mkdir -p $@

# ---------------------------------------------------------------- QNX targets

$(OUT)/hello: samples/hello/hello.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/msgpass: samples/msgpass/msgpass.c common/arb_pulses.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/test_libcontract: tests/test_libcontract.c tests/arb_test.h $(LIBCONTRACT_SRC) $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_libcontract.c $(LIBCONTRACT_SRC) $(LDLIBS)

# --------------------------------------------------------------- host checks

$(HOST_OUT)/test_libcontract: tests/test_libcontract.c tests/arb_test.h $(LIBCONTRACT_SRC) $(LIBCONTRACT_HDR) | $(HOST_OUT)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_libcontract.c $(LIBCONTRACT_SRC) -lm

host-test: $(HOST_OUT)/test_libcontract
	$(HOST_OUT)/test_libcontract

analyze: | $(HOST_OUT)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fanalyzer $(INCLUDES) -c $(LIBCONTRACT_SRC) -o $(HOST_OUT)/arb_contract.analyze.o
	cppcheck --std=c11 --enable=warning,style,performance,portability --inline-suppr \
	         --error-exitcode=1 --quiet $(INCLUDES) $(LIBCONTRACT_SRC)

# Purity (NFR-K2): compile the library alone and list every symbol it needs from
# outside. Anything beyond the allowed C library set fails.
check-pure: qnx-env | $(HOST_OUT)
	mkdir -p $(OUT)/obj
	$(HOST_CC) -std=c11 -O2 $(INCLUDES) -c $(LIBCONTRACT_SRC) -o $(HOST_OUT)/arb_contract.o
	$(CC) $(CFLAGS) -c $(LIBCONTRACT_SRC) -o $(OUT)/obj/arb_contract.o
	../scripts/check-pure.sh nm $(HOST_OUT)/arb_contract.o
	../scripts/check-pure.sh $(QNX_NM_$(ARCH)) $(OUT)/obj/arb_contract.o

clean:
	rm -rf $(OUT) $(HOST_OUT)
