# Build Arbiter's QNX binaries with the SDP 8.0 toolchain, and the host-side
# checks of the pure library.
#
#   source ~/qnx800/qnxsdp-env.sh
#   make -C qnx ARCH=x86_64        # QEMU
#   make -C qnx ARCH=aarch64le     # Raspberry Pi 5
#   make -C qnx host-test          # libcontract unit tests on the host, with sanitizers
#   make -C qnx analyze            # gcc -fanalyzer and cppcheck over libcontract
#   make -C qnx analyze-qnx        # qcc -fanalyzer and cppcheck over the guardian
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
INCLUDES := -Icommon -Ilibcontract -Itests -Iarm
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -D_QNX_SOURCE $(INCLUDES)
LDLIBS   := -lm
# NFR-Q6: the trusted processes log through slogger2 (slog2_register page: "-l slog2").
# QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/slog2_register.html
SLOG2_LIBS := -lslog2
OUT      := ../build/$(ARCH)

HOST_CC     ?= gcc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -O1 -g \
               -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all $(INCLUDES)
HOST_OUT    := ../build/host

LIBCONTRACT_SRC := libcontract/arb_contract.c
LIBCONTRACT_HDR := libcontract/arb_contract.h common/arb_contract_table.h common/arb_proto.h common/arb_arm_model.h

BINS := $(OUT)/hello $(OUT)/msgpass $(OUT)/test_libcontract $(OUT)/test_contract_file $(OUT)/test_send_timeout \
        $(OUT)/arb-guardian $(OUT)/arb_arm_stub $(OUT)/test_guardian $(OUT)/test_logd $(OUT)/guardian_suite.sh \
        $(OUT)/arb-arm $(OUT)/test_arm_motion $(OUT)/test_arm $(OUT)/test_e2e $(OUT)/test_wd_scoid $(OUT)/arm_suite.sh \
        $(OUT)/arb-watchdog $(OUT)/test_watchdog $(OUT)/watchdog_suite.sh \
        $(OUT)/arb-inject $(OUT)/arb-stall-probe $(OUT)/bench_scenario.sh $(OUT)/arb-ctl $(OUT)/arb-untrusted $(OUT)/f11_suite.sh $(OUT)/policy_suite.sh $(OUT)/arb_suite_lib.sh

.PHONY: all clean qnx-env host-test analyze analyze-qnx check-pure
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

$(OUT)/test_contract_file: tests/test_contract_file.c $(LIBCONTRACT_SRC) $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_contract_file.c $(LIBCONTRACT_SRC) $(LDLIBS)

$(OUT)/test_send_timeout: tests/test_send_timeout.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

GUARDIAN_SRC := guardian/main.c guardian/arm_link.c common/arb_wd_client.c common/arb_privdrop.c common/arb_server_check.c
GUARDIAN_HDR := common/arb_decisions.h common/arb_log.h common/arb_own_pulse.h common/arb_hex.h guardian/arm_link.h common/arb_arm_proto.h common/arb_devctl.h common/arb_prio.h common/arb_pulses.h \
                common/arb_wd_client.h common/arb_watchdog_proto.h common/arb_ids.h common/arb_privdrop.h common/arb_server_check.h

$(OUT)/arb-guardian: $(GUARDIAN_SRC) $(GUARDIAN_HDR) $(LIBCONTRACT_SRC) $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -Iguardian -o $@ $(GUARDIAN_SRC) $(LIBCONTRACT_SRC) $(LDLIBS) $(SLOG2_LIBS)

$(OUT)/arb_arm_stub: tests/arb_arm_stub.c common/arb_arm_proto.h common/arb_ids.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/test_guardian: tests/test_guardian.c tests/arb_test.h tests/arb_as_gateway.h common/arb_ids.h common/arb_devctl.h common/arb_decisions.h $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/test_logd: tests/test_logd.c tests/arb_test.h tests/arb_as_gateway.h common/arb_devctl.h common/arb_decisions.h $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_logd.c

$(OUT)/guardian_suite.sh: tests/guardian_suite.sh | $(OUT)
	install -m 755 $< $@

ARM_SRC := arm/main.c arm/arm_motion.c arm/arm_backend_sim.c common/arb_wd_client.c common/arb_privdrop.c common/arb_server_check.c
ARM_HDR := common/arb_log.h common/arb_own_pulse.h common/arb_hex.h arm/arm_motion.h arm/arm_backend.h common/arb_arm_model.h common/arb_arm_proto.h \
           common/arb_telemetry.h common/arb_prio.h common/arb_pulses.h common/arb_wd_client.h common/arb_watchdog_proto.h \
           common/arb_ids.h common/arb_privdrop.h common/arb_server_check.h

$(OUT)/arb-arm: $(ARM_SRC) $(ARM_HDR) $(LIBCONTRACT_SRC) $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(ARM_SRC) $(LIBCONTRACT_SRC) $(LDLIBS) $(SLOG2_LIBS)

$(OUT)/test_arm_motion: tests/test_arm_motion.c tests/arb_test.h arm/arm_motion.c arm/arm_motion.h arm/arm_backend_sim.c arm/arm_backend.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_arm_motion.c arm/arm_motion.c arm/arm_backend_sim.c $(LDLIBS)

$(OUT)/test_arm: tests/test_arm.c tests/arb_test.h $(ARM_HDR) $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/test_e2e: tests/test_e2e.c tests/arb_test.h tests/arb_as_gateway.h common/arb_ids.h $(ARM_HDR) common/arb_devctl.h $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/test_wd_scoid: tests/test_wd_scoid.c tests/arb_test.h common/arb_privdrop.c $(ARM_HDR) common/arb_devctl.h $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_wd_scoid.c common/arb_privdrop.c $(LDLIBS)

$(OUT)/arm_suite.sh: tests/arm_suite.sh | $(OUT)
	install -m 755 $< $@

WATCHDOG_SRC := watchdog/main.c common/arb_privdrop.c
WATCHDOG_HDR := common/arb_log.h common/arb_own_pulse.h common/arb_watchdog_proto.h common/arb_prio.h common/arb_pulses.h \
                common/arb_ids.h common/arb_privdrop.h

$(OUT)/arb-watchdog: $(WATCHDOG_SRC) $(WATCHDOG_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(WATCHDOG_SRC) $(SLOG2_LIBS)

$(OUT)/test_watchdog: tests/test_watchdog.c tests/arb_test.h common/arb_wd_client.c common/arb_wd_client.h common/arb_server_check.c common/arb_server_check.h $(WATCHDOG_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_watchdog.c common/arb_wd_client.c common/arb_server_check.c

$(OUT)/watchdog_suite.sh: tests/watchdog_suite.sh | $(OUT)
	install -m 755 $< $@

# Operator console (untrusted; FR-W4, FR-G5).
$(OUT)/arb-ctl: tools/arb_ctl.c common/arb_devctl.h $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

# Fault bench client (untrusted; run by `arbiter bench run`).
$(OUT)/arb-inject: tests/arb_inject.c common/arb_ids.h common/arb_telemetry.h common/arb_devctl.h common/arb_watchdog_proto.h $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/bench_scenario.sh: tests/bench_scenario.sh | $(OUT)
	install -m 755 $< $@

# F11 attacker (untrusted; run by f11_suite.sh as user ARB_UID_UNTRUSTED).
$(OUT)/arb-stall-probe: tests/arb_stall_probe.c tests/arb_as_gateway.h common/arb_ids.h common/arb_telemetry.h common/arb_devctl.h common/arb_watchdog_proto.h $(LIBCONTRACT_HDR) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/arb_stall_probe.c $(LDLIBS)

$(OUT)/arb-untrusted: tests/arb_untrusted.c common/arb_arm_proto.h common/arb_telemetry.h common/arb_devctl.h common/arb_ids.h common/arb_watchdog_proto.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/f11_suite.sh: tests/f11_suite.sh | $(OUT)
	install -m 755 $< $@

$(OUT)/policy_suite.sh: tests/policy_suite.sh | $(OUT)
	install -m 755 $< $@

# Sourced by the suites; executable so deploy.sh copies it.
$(OUT)/arb_suite_lib.sh: tests/arb_suite_lib.sh | $(OUT)
	install -m 755 $< $@

# --------------------------------------------------------------- host checks

$(HOST_OUT)/test_libcontract: tests/test_libcontract.c tests/arb_test.h $(LIBCONTRACT_SRC) $(LIBCONTRACT_HDR) | $(HOST_OUT)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_libcontract.c $(LIBCONTRACT_SRC) -lm

$(HOST_OUT)/test_arm_motion: tests/test_arm_motion.c tests/arb_test.h arm/arm_motion.c arm/arm_motion.h arm/arm_backend_sim.c arm/arm_backend.h common/arb_arm_model.h | $(HOST_OUT)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/test_arm_motion.c arm/arm_motion.c arm/arm_backend_sim.c -lm

host-test: $(HOST_OUT)/test_libcontract $(HOST_OUT)/test_arm_motion
	$(HOST_OUT)/test_libcontract
	$(HOST_OUT)/test_arm_motion

analyze: | $(HOST_OUT)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fanalyzer $(INCLUDES) -c $(LIBCONTRACT_SRC) -o $(HOST_OUT)/arb_contract.analyze.o
	cppcheck --std=c11 --enable=warning,style,performance,portability --inline-suppr \
	         --error-exitcode=1 --quiet $(INCLUDES) $(LIBCONTRACT_SRC)

# Trusted QNX code (rt-c-review): qcc's gcc 12 static analyzer with extra
# warnings, and cppcheck. Needs the SDP environment.
analyze-qnx: qnx-env
	$(CC) -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -fanalyzer -O2 -D_QNX_SOURCE $(INCLUDES) -Iguardian -c guardian/main.c -o /dev/null
	$(CC) -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -fanalyzer -O2 -D_QNX_SOURCE $(INCLUDES) -Iguardian -c guardian/arm_link.c -o /dev/null
	for f in $(ARM_SRC) $(WATCHDOG_SRC); do $(CC) -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow -fanalyzer -O2 -D_QNX_SOURCE $(INCLUDES) -c $$f -o /dev/null || exit 1; done
	cppcheck --std=c11 --enable=warning,style,performance,portability --inline-suppr \
	         --error-exitcode=1 --quiet -D_QNX_SOURCE $(INCLUDES) -Iguardian $(GUARDIAN_SRC) $(ARM_SRC) $(WATCHDOG_SRC)

# Purity (NFR-K2): compile the library alone and list every symbol it needs from
# outside. Anything beyond the allowed C library set fails.
check-pure: qnx-env | $(HOST_OUT)
	mkdir -p $(OUT)/obj
	$(HOST_CC) -std=c11 -O2 $(INCLUDES) -c $(LIBCONTRACT_SRC) -o $(HOST_OUT)/arb_contract.o
	$(CC) $(CFLAGS) -c $(LIBCONTRACT_SRC) -o $(OUT)/obj/arb_contract.o
	$(HOST_CC) -std=c11 -O2 $(INCLUDES) -c arm/arm_motion.c -o $(HOST_OUT)/arm_motion.o
	$(CC) $(CFLAGS) -c arm/arm_motion.c -o $(OUT)/obj/arm_motion.o
	../scripts/check-pure.sh nm $(HOST_OUT)/arb_contract.o
	../scripts/check-pure.sh $(QNX_NM_$(ARCH)) $(OUT)/obj/arb_contract.o
	../scripts/check-pure.sh nm $(HOST_OUT)/arm_motion.o
	../scripts/check-pure.sh $(QNX_NM_$(ARCH)) $(OUT)/obj/arm_motion.o

clean:
	rm -rf $(OUT) $(HOST_OUT)
