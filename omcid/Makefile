# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

# The build of omcid: the daemon, its client library, its tools and its
# tests.
#
#   make                   the daemon and the tools, in build/
#   make check             the tests, and that the class table is what
#                          data/g988-me-min.tsv gives
#   make coverage          the tests on an instrumented build, failing
#                          unless every line of src/ ran
#   make check-style       clang-format and checkpatch over the sources
#                          (CLANG_FORMAT= and CHECKPATCH= name the tools)
#   make board             the daemon and the client tools for the board, on
#                          the kernel backend, in build-board/board/;
#                          UBUS=1 puts the daemon on ubus (libubus,
#                          libubox)
#
# CROSS_COMPILE= builds for another machine; V=1 shows the commands.

SHELL := /bin/sh
.DEFAULT_GOAL := all

# The compiler and archiver follow CROSS_COMPILE unless given outright.
CROSS_COMPILE ?=
ifneq ($(CROSS_COMPILE),)
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
endif
ifeq ($(origin AR),default)
AR := $(CROSS_COMPILE)ar
endif

# SANITIZE=auto builds the native objects with AddressSanitizer and
# UndefinedBehaviorSanitizer when the compiler can; PREFIX_MAP=1 keeps the
# build directory out of the objects.
CFLAGS     ?= -O2 -g -DNDEBUG
SANITIZE   ?= auto
PREFIX_MAP ?= 1
V          ?= 0
ifeq ($(origin ARFLAGS),default)
ARFLAGS := rcsD
endif

# A build for the board lives in its own directory and has no sanitizers.
ifeq ($(BOARD),1)
BUILD    ?= build-board
SANITIZE := no
endif

# A coverage build lives in its own directory.
ifneq ($(filter coverage,$(MAKECMDGOALS)),)
COVERAGE ?= 1
BUILD    ?= build-cov
ifneq ($(COVERAGE),1)
$(error the coverage goal needs COVERAGE=1)
endif
endif
COVERAGE ?= 0
BUILD    ?= build

ifeq ($(V),1)
Q :=
say = @:
else
Q := @
say = @echo "  $(1)	$(2)"
endif

# The instrumentation of each group of objects: coverage, sanitizers or none.
ifeq ($(COVERAGE),1)
SAN := no
else ifeq ($(SANITIZE),auto)
ifneq ($(CROSS_COMPILE),)
SAN := no
else
SAN := $(shell d=$$(mktemp -d) && printf 'int main(void) { return 0; }\n' > $$d/p.c && \
               $(CC) -fsanitize=address,undefined -o $$d/p $$d/p.c >/dev/null 2>&1 && $$d/p >/dev/null 2>&1 && \
               echo yes || echo no; rm -rf $$d)
endif
else
SAN := $(SANITIZE)
endif

ifeq ($(COVERAGE),1)
INSTR_CORE   := --coverage -O0 -fno-inline
LINK_CORE    := --coverage
INSTR_CLIENT := $(INSTR_CORE)
LINK_CLIENT  := $(LINK_CORE)
else ifeq ($(SAN),yes)
INSTR_CORE   := -fsanitize=address,undefined
LINK_CORE    := -fsanitize=address,undefined
INSTR_CLIENT := -fsanitize=address,undefined
LINK_CLIENT  := -fsanitize=address,undefined
else
INSTR_CORE   :=
LINK_CORE    :=
INSTR_CLIENT :=
LINK_CLIENT  :=
endif

ifeq ($(PREFIX_MAP)$(COVERAGE),10)
MAP := -ffile-prefix-map=$(CURDIR)=. -ffile-prefix-map=$(abspath $(BUILD))=.
else
MAP :=
endif

# Every build uses the same language, warnings and include paths. This
# build runs on the host backend, which stands in for the hardware.
override STD  := -std=gnu11
override WARN := -Wall -Wextra -Wmissing-prototypes -Wstrict-prototypes -Wshadow -Wpointer-arith \
                 -Wwrite-strings
override DEFS := $(if $(filter 1,$(BOARD)),,-DOMCID_HW_STUB=1) $(if $(filter 1,$(UBUS)),-DOMCID_UBUS=1)
override INCS := -Iinclude -Isrc -Itarget

COMPILE = $(CC) $(STD) $(CPPFLAGS) $(INCS) $(DEFS) $(CFLAGS) $(WARN) $(MAP) $(GROUP_FLAGS) -MMD -MP

# The daemon's core, its client library and its entry point.
CORE_SRC := \
	src/daemon.c \
	src/hw/hw.c \
	src/hw/hw_host.c \
	src/mcast/acl.c \
	src/mcast/agent.c \
	src/mcast/report.c \
	src/me/action.c \
	src/me/bridge.c \
	src/me/defaults.c \
	src/me/fdb.c \
	src/me/filter.c \
	src/me/gem.c \
	src/me/layout.c \
	src/me/pm.c \
	src/me/table.c \
	src/me/tables.c \
	src/me/upload.c \
	src/me/values.c \
	src/me/vlan.c \
	src/mib/store.c \
	src/omci/alarm.c \
	src/omci/ber.c \
	src/omci/cases.c \
	src/omci/clock.c \
	src/omci/engine.c \
	src/omci/frame.c \
	src/omci/notify.c \
	src/omci/optical.c \
	src/omci/status.c \
	src/omci/test.c \
	src/sys/ctl.c \
	src/sys/log.c \
	src/sys/publish.c \
	src/sys/transport_pipe.c

CLIENT_SRC := src/libomcid/libomcid.c

DAEMON_SRC := src/main.c

# The board's own sources: the kernel backend, the OMCI port, its entry point,
# and with UBUS=1 the daemon on ubus.
UBUS       ?= 0
TARGET_SRC := target/hw_kernel.c target/main.c target/transport_port.c
UBUS_SRC   := target/bus_ubus.c

# The tools: those linked with the core, and those that only talk to the
# control socket.
TOOLS_CORE   := omcisweep omcialarm
TOOLS_CLIENT := omcitap omcivlan omci_ctl_probe

tools_src_omcisweep       := tools/omcisweep.c
tools_src_omcialarm       := tools/omcialarm.c
tools_src_omcitap         := tools/omcitap.c
tools_src_omcivlan        := tools/omcivlan.c
tools_src_omci_ctl_probe  := tools/deploy/omci_ctl_probe.c

# The tests, and what some of them take as argument: the data table or the
# daemon itself.
TESTS := actions alarm attr_sweep avc_origin ber bridge cases cases_mib checks client clock ctl ctl_mode daemon defaults edges engine_edges exec filter frame gem getnext \
         hw_host hw_ops keep log mcast mcast_agent me171 me_edges mib_data_sync mib_store mibreset_reply model msgtype_matrix msgtypes multibit optical pm pipe priority publish \
         roundtrip std_classes swdl synctime_stamp tables tap test_instance testresult uni_rate upload vlan vlan_pri
CLIENT_TESTS := client ctl
SCRIPT_TESTS := coverage_gate style_gate
TSV_TESTS    := actions attr_sweep defaults model roundtrip tables upload
TSV          := $(abspath data/g988-me-min.tsv)
PROG_TESTS   := exec
PROG         := $(abspath $(BUILD))/omcid
test_args     = $(if $(filter $(1),$(TSV_TESTS)),$(TSV))$(if $(filter $(1),$(PROG_TESTS)),$(PROG))

# Tests that make system calls fail link with these calls wrapped.
comma       := ,
WRAP_CALLS  := socket bind listen accept connect poll read write send recv fcntl clock_gettime setsockopt
WRAP_FLAGS  := $(foreach c,$(WRAP_CALLS),-Wl$(comma)--wrap=$(c))
FAULT_TESTS := client ctl daemon pipe
HELPER_SRC  := tests/fault.c

# check-lists fails when a file in the tree is missing from the lists above,
# or the lists name a file that is not there.
LISTED_SRC := $(sort $(CORE_SRC) $(CLIENT_SRC) $(DAEMON_SRC))
FOUND_SRC  := $(sort $(wildcard src/*.c src/*/*.c))
LISTED_TARGET := $(sort $(TARGET_SRC) $(UBUS_SRC))
FOUND_TARGET  := $(sort $(wildcard target/*.c))
LISTED_TOOLS := $(sort $(foreach t,$(TOOLS_CORE) $(TOOLS_CLIENT),$(tools_src_$(t))))
FOUND_TOOLS  := $(sort $(wildcard tools/*.c tools/*/*.c))
LISTED_TESTS := $(sort $(foreach t,$(TESTS),tests/test_$(t).c))
FOUND_TESTS  := $(sort $(wildcard tests/test_*.c))
LISTED_HELPERS := $(sort $(HELPER_SRC))
FOUND_HELPERS  := $(filter-out $(FOUND_TESTS),$(sort $(wildcard tests/*.c)))
LISTED_CHECKS := $(sort $(foreach t,$(SCRIPT_TESTS),tools/check_$(t).sh))
FOUND_CHECKS  := $(sort $(wildcard tools/check_*.sh))

CLANG_FORMAT ?=
CHECKPATCH   ?=
STYLE_GATE   := tools/style_gate.sh
STYLE_OPEN   := tools/style-open.txt
LISTED_STYLE := $(sort $(STYLE_GATE) $(STYLE_OPEN) .clang-format .checkpatch.conf)
FOUND_STYLE  := $(sort $(wildcard tools/style* .clang-format .checkpatch.conf))

# Objects, libraries, programs and tests.
OBJ := $(BUILD)/obj

CORE_OBJS   := $(CORE_SRC:%.c=$(OBJ)/%.o)
CLIENT_OBJS := $(CLIENT_SRC:%.c=$(OBJ)/%.o)
DAEMON_OBJS := $(DAEMON_SRC:%.c=$(OBJ)/%.o)
TOOL_OBJS   := $(foreach t,$(TOOLS_CORE) $(TOOLS_CLIENT),$(tools_src_$(t):%.c=$(OBJ)/%.o))
TEST_OBJS   := $(foreach t,$(TESTS),$(OBJ)/tests/test_$(t).o)
HELPER_OBJS := $(HELPER_SRC:%.c=$(OBJ)/%.o)
ALL_OBJS    := $(CORE_OBJS) $(CLIENT_OBJS) $(DAEMON_OBJS) $(TOOL_OBJS) $(TEST_OBJS) $(HELPER_OBJS)

CORE_LIB   := $(BUILD)/libomcid_core.a
CLIENT_LIB := $(BUILD)/libomcid.a

PROGRAMS   := $(BUILD)/omcid $(addprefix $(BUILD)/,$(TOOLS_CORE) $(TOOLS_CLIENT))
TEST_BINS  := $(addprefix $(BUILD)/test_,$(TESTS))

$(CLIENT_OBJS) $(foreach t,$(TOOLS_CLIENT),$(tools_src_$(t):%.c=$(OBJ)/%.o)): GROUP_FLAGS := $(INSTR_CLIENT)
$(CORE_OBJS) $(DAEMON_OBJS) $(TEST_OBJS) $(HELPER_OBJS) $(foreach t,$(TOOLS_CORE),$(tools_src_$(t):%.c=$(OBJ)/%.o)): GROUP_FLAGS := $(INSTR_CORE)

.PHONY: all tests check check-lists check-gen check-style list-tests coverage clean

all: check-lists $(CORE_LIB) $(CLIENT_LIB) $(PROGRAMS)

tests: check-lists $(TEST_BINS)

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(call say,CC,$<)
	$(Q)$(COMPILE) -c $< -o $@

$(CORE_LIB): $(CORE_OBJS)
	@rm -f $@
	$(call say,AR,$@)
	$(Q)$(AR) $(ARFLAGS) $@ $^

$(CLIENT_LIB): $(CLIENT_OBJS)
	@rm -f $@
	$(call say,AR,$@)
	$(Q)$(AR) $(ARFLAGS) $@ $^

LINK_EXTRA :=
LINK = $(CC) $(LDFLAGS) $(LINK_EXTRA) -o $@ $(filter %.o,$^)

$(BUILD)/omcid: $(DAEMON_OBJS) $(CORE_LIB)
	$(call say,LD,$@)
	$(Q)$(LINK) $(LINK_CORE) $(CORE_LIB) $(LDLIBS)

define core_tool
$$(BUILD)/$(1): $$(OBJ)/$$(tools_src_$(1):%.c=%.o) $$(CORE_LIB)
	$$(call say,LD,$$@)
	$$(Q)$$(LINK) $$(LINK_CORE) $$(CORE_LIB) $$(LDLIBS)
endef
define client_tool
$$(BUILD)/$(1): $$(OBJ)/$$(tools_src_$(1):%.c=%.o) $$(CLIENT_LIB)
	$$(call say,LD,$$@)
	$$(Q)$$(LINK) $$(LINK_CLIENT) $$(CLIENT_LIB) $$(LDLIBS)
endef
$(foreach t,$(TOOLS_CORE),$(eval $(call core_tool,$(t))))
$(foreach t,$(TOOLS_CLIENT),$(eval $(call client_tool,$(t))))

define core_test
$$(BUILD)/test_$(1): $$(OBJ)/tests/test_$(1).o $$(CORE_LIB)
	$$(call say,LD,$$@)
	$$(Q)$$(LINK) $$(LINK_CORE) $$(CORE_LIB) $$(LDLIBS)
endef
define client_test
$$(BUILD)/test_$(1): $$(OBJ)/tests/test_$(1).o $$(CORE_LIB) $$(CLIENT_LIB)
	$$(call say,LD,$$@)
	$$(Q)$$(LINK) $$(LINK_CORE) $$(CORE_LIB) $$(CLIENT_LIB) $$(LDLIBS)
endef
$(foreach t,$(filter-out $(CLIENT_TESTS),$(TESTS)),$(eval $(call core_test,$(t))))
$(foreach t,$(CLIENT_TESTS),$(eval $(call client_test,$(t))))
$(foreach t,$(PROG_TESTS),$(eval $(BUILD)/test_$(t): $(BUILD)/omcid))
$(foreach t,$(FAULT_TESTS),$(eval $(BUILD)/test_$(t): $(HELPER_OBJS)))
$(foreach t,$(FAULT_TESTS),$(eval $(BUILD)/test_$(t): LINK_EXTRA := $(WRAP_FLAGS)))

# Compare the lists of files with the tree.
check-lists:
	@bad=0; \
	for set in SRC TARGET TOOLS TESTS HELPERS CHECKS STYLE; do \
	    case $$set in \
	        SRC)     listed='$(LISTED_SRC)';     found='$(FOUND_SRC)' ;; \
	        TARGET)  listed='$(LISTED_TARGET)';  found='$(FOUND_TARGET)' ;; \
	        TOOLS)   listed='$(LISTED_TOOLS)';   found='$(FOUND_TOOLS)' ;; \
	        TESTS)   listed='$(LISTED_TESTS)';   found='$(FOUND_TESTS)' ;; \
	        HELPERS) listed='$(LISTED_HELPERS)'; found='$(FOUND_HELPERS)' ;; \
	        CHECKS)  listed='$(LISTED_CHECKS)';  found='$(FOUND_CHECKS)' ;; \
	        STYLE)   listed='$(LISTED_STYLE)';   found='$(FOUND_STYLE)' ;; \
	    esac; \
	    for f in $$found; do \
	        case " $$listed " in *" $$f "*) ;; *) echo "Makefile: $$f is not listed ($$set)" >&2; bad=1 ;; esac; \
	    done; \
	    for f in $$listed; do \
	        case " $$found " in *" $$f "*) ;; *) echo "Makefile: $$f is listed but not there ($$set)" >&2; bad=1 ;; esac; \
	    done; \
	done; \
	[ $$bad -eq 0 ]

list-tests:
	@$(foreach t,$(TESTS),printf '%s\t./test_%s %s\n' '$(t)' '$(t)' '$(call test_args,$(t))';) \
	 $(foreach t,$(SCRIPT_TESTS),printf '%s\tsh %s\n' '$(t)' '$(abspath tools/check_$(t).sh)';)

check-gen:
	$(Q)python3 tools/gen_me_table.py --check

check-style:
	$(Q)sh $(STYLE_GATE) --clang-format '$(CLANG_FORMAT)' --checkpatch '$(CHECKPATCH)' --open $(STYLE_OPEN) -- \
	    $$(find include src target tests tools -name '*.c' -o -name '*.h' | sort)

# Run every test; print the end of the log of each one that fails.
check: check-gen tests
	@cd $(BUILD) && total=0 && failed=0 && \
	for t in $(TESTS) $(SCRIPT_TESTS); do \
	    total=$$((total + 1)); \
	    arg=; \
	    case " $(TSV_TESTS) " in *" $$t "*) arg="$(TSV)" ;; esac; \
	    case " $(PROG_TESTS) " in *" $$t "*) arg="$(PROG)" ;; esac; \
	    case " $(SCRIPT_TESTS) " in \
	        *" $$t "*) cmd="sh $(abspath tools)/check_$$t.sh" ;; \
	        *)         cmd="./test_$$t $$arg" ;; \
	    esac; \
	    if $$cmd > "$$t.log" 2>&1; then echo "PASS  $$t"; \
	    else echo "FAIL  $$t"; tail -n 20 "$$t.log" | sed 's/^/      | /'; failed=$$((failed + 1)); fi; \
	done; \
	echo "$$total checks, $$failed failed"; \
	[ $$failed -eq 0 ]

# Run the tests on a coverage build and judge what they reached.
coverage: all tests
	@echo "-- coverage: discarding stale .gcda"
	$(Q)find $(BUILD) -name '*.gcda' -delete
	$(Q)$(MAKE) --no-print-directory check BUILD=$(BUILD) COVERAGE=$(COVERAGE)
	@echo "-- coverage: measuring"
	$(Q)python3 tools/coverage_gate.py --build $(abspath $(BUILD)) --root $(CURDIR)

# The board: the core without the host backend, as an archive so that only
# what the programs use is linked, and the kernel backend of target/.
BOARD_CORE_SRC := $(filter-out src/hw/hw_host.c,$(CORE_SRC))
BOARD_LIB      := $(BUILD)/libomcid_board.a
BOARD_PROGRAMS := $(addprefix $(BUILD)/board/,omcid omcitap omcivlan)
BOARD_SRC      := $(TARGET_SRC) $(if $(filter 1,$(UBUS)),$(UBUS_SRC))
BOARD_LIBS     := $(if $(filter 1,$(UBUS)),-lubus -lubox)

.PHONY: board
ifeq ($(BOARD),1)
board: $(BOARD_PROGRAMS)

$(BOARD_LIB): $(BOARD_CORE_SRC:%.c=$(OBJ)/%.o)
	@rm -f $@
	$(call say,AR,$@)
	$(Q)$(AR) $(ARFLAGS) $@ $^

$(BUILD)/board/omcid: $(BOARD_SRC:%.c=$(OBJ)/%.o) $(BOARD_LIB)
	@mkdir -p $(dir $@)
	$(call say,LD,$@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(BOARD_LIB) $(BOARD_LIBS) $(LDLIBS)

$(BUILD)/board/omcitap $(BUILD)/board/omcivlan: $(BUILD)/board/%: $(OBJ)/tools/%.o $(CLIENT_LIB)
	@mkdir -p $(dir $@)
	$(call say,LD,$@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $< $(CLIENT_LIB) $(LDLIBS)
else
board:
	$(Q)$(MAKE) --no-print-directory BOARD=1 board
endif

# The kernel backend against the kernel the host runs, in a network namespace
# of its own, with dummy interfaces for the switch's ports; its filters go
# into software (OMCID_TC_FLAGS=0), as no switch is behind a dummy interface.
# It needs root, network namespaces and tc, and says that it skipped without
# them (tests/kernel/test_hw_kernel.c).
.PHONY: check-kernel
ifeq ($(BOARD),1)
KERNEL_TEST := $(BUILD)/test_hw_kernel

check-kernel: $(KERNEL_TEST)
	$(KERNEL_TEST)

$(OBJ)/kernel-sw/hw_kernel.o: target/hw_kernel.c
	@mkdir -p $(dir $@)
	$(call say,CC,$< (filters in software))
	$(Q)$(COMPILE) -DOMCID_TC_FLAGS=0 -c $< -o $@

$(KERNEL_TEST): $(OBJ)/tests/kernel/test_hw_kernel.o $(OBJ)/kernel-sw/hw_kernel.o $(BOARD_LIB)
	$(call say,LD,$@)
	$(Q)$(CC) $(LDFLAGS) -o $@ $(filter %.o,$^) $(BOARD_LIB) $(LDLIBS)
else
check-kernel:
	$(Q)$(MAKE) --no-print-directory BOARD=1 BUILD=build-kernel check-kernel
endif

clean:
	rm -rf $(BUILD)

-include $(ALL_OBJS:.o=.d)
