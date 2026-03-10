# DoIP Server Makefile
#
# Usage:
#   make                          # Build for Ubuntu (default)
#   make PLATFORM=torizon         # Build for Torizon/Verdin iMX8MPlus
#   make PLATFORM=torizon CROSS_COMPILE=/opt/qconnect_sdk_musl/bin/arm-linux-
#   make test                     # Run discovery tests
#   make ci-test                  # Run all tests
#   make install                  # Install server + scripts

# Platform selection (ubuntu or torizon)
PLATFORM ?= ubuntu

ifneq ($(PLATFORM),ubuntu)
ifneq ($(PLATFORM),torizon)
$(error PLATFORM must be 'ubuntu' or 'torizon' (got '$(PLATFORM)'))
endif
endif

ifeq ($(PLATFORM),torizon)
  # QConnect SDK — same toolchain as Fleet-Connect-1
  # Toradex Verdin iMX8MPlus, Torizon Linux, musl libc
  CROSS_COMPILE ?= /opt/qconnect_sdk_musl/bin/arm-linux-
  CC       = $(CROSS_COMPILE)gcc
  OPT      = -Os
  PLAT_DEF = -DPLATFORM_TORIZON
else
  CC       = gcc
  OPT      = -O2
  PLAT_DEF = -DPLATFORM_UBUNTU
endif

CFLAGS   = -Wall -Wextra -Wpedantic $(OPT) -g -std=c11 $(PLAT_DEF)
INCLUDES = -Iinclude
LDFLAGS  = -lpthread

# Phone-home support (standalone HMAC-SHA256, no external crypto dependency)
PHONEHOME_SRCS = src/phonehome_handler.c src/hmac_sha256.c

# New: CLI + script generation
CLI_SRCS = src/cli.c src/script_gen.c

SERVER_SRCS      = src/main.c src/doip.c src/doip_server.c src/config.c src/doip_log.c $(PHONEHOME_SRCS) $(CLI_SRCS)
TEST_SRCS        = test/test_discovery.c src/doip.c src/doip_client.c src/config.c src/doip_log.c
TEST_SERVER_SRCS = test/test_server.c src/doip.c src/doip_client.c src/config.c src/doip_log.c
TEST_PH_SRCS     = test/test_phonehome.c src/phonehome_handler.c src/hmac_sha256.c src/doip_log.c

SERVER_TARGET  = doip-server
TEST_TARGET    = test-discovery
TEST_SERVER    = test-server
TEST_PH_TARGET = test-phonehome

PREFIX ?= /usr

.PHONY: all clean test test-config test-full run-test-phonehome ci-test install install-systemd install-initd

all: $(SERVER_TARGET) $(TEST_TARGET) $(TEST_SERVER) $(TEST_PH_TARGET)

$(SERVER_TARGET): $(SERVER_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

$(TEST_TARGET): $(TEST_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

$(TEST_PH_TARGET): $(TEST_PH_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

test: $(SERVER_TARGET) $(TEST_TARGET)
	@echo "Starting server..."
	@./$(SERVER_TARGET) -c doip-server.conf &
	@SERVER_PID=$$!; \
	trap 'kill $$SERVER_PID 2>/dev/null; wait $$SERVER_PID 2>/dev/null' EXIT; \
	for i in 1 2 3 4 5 6 7 8 9 10; do \
		nc -z 127.0.0.1 13400 2>/dev/null && break; \
		sleep 0.5; \
	done; \
	echo "Running discovery tests..."; \
	timeout 60 ./$(TEST_TARGET) -c doip-server.conf 127.0.0.1 13400; \
	TEST_RESULT=$$?; \
	kill $$SERVER_PID 2>/dev/null; wait $$SERVER_PID 2>/dev/null; \
	trap - EXIT; \
	exit $$TEST_RESULT

run-test-phonehome: $(TEST_PH_TARGET)
	@echo "Running phone-home unit tests..."
	@./$(TEST_PH_TARGET)

$(TEST_SERVER): $(TEST_SERVER_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)

test-config: $(SERVER_TARGET) $(TEST_TARGET)
	@bash test/test_config.sh

test-full: $(SERVER_TARGET) $(TEST_SERVER)
	@echo "Starting server..."
	@./$(SERVER_TARGET) -c doip-server.conf &
	@SERVER_PID=$$!; \
	trap 'kill $$SERVER_PID 2>/dev/null; wait $$SERVER_PID 2>/dev/null' EXIT; \
	for i in 1 2 3 4 5 6 7 8 9 10; do \
		nc -z 127.0.0.1 13400 2>/dev/null && break; \
		sleep 0.5; \
	done; \
	echo "Running full test suite..."; \
	timeout 180 ./$(TEST_SERVER) -c doip-server.conf 127.0.0.1 13400; \
	TEST_RESULT=$$?; \
	kill $$SERVER_PID 2>/dev/null; wait $$SERVER_PID 2>/dev/null; \
	trap - EXIT; \
	exit $$TEST_RESULT

ci-test: $(SERVER_TARGET) $(TEST_TARGET) $(TEST_SERVER) $(TEST_PH_TARGET)
	@echo "=== Phone-Home Unit Tests ==="
	@$(MAKE) run-test-phonehome
	@echo ""
	@echo "=== Smoke Tests ==="
	@$(MAKE) test
	@echo ""
	@echo "=== Full Server Tests ==="
	@SKIP_TIMEOUT=1 $(MAKE) test-full

install: all
	install -D -m 755 $(SERVER_TARGET) $(DESTDIR)$(PREFIX)/sbin/doip-server
	install -D -m 755 scripts/phonehome-keygen.sh $(DESTDIR)$(PREFIX)/sbin/phonehome-keygen.sh
	install -D -m 755 scripts/phonehome-register.sh $(DESTDIR)$(PREFIX)/sbin/phonehome-register.sh
	install -D -m 755 scripts/phonehome-connect.sh $(DESTDIR)$(PREFIX)/sbin/phonehome-connect.sh
	install -d -m 700 $(DESTDIR)/etc/phonehome
	install -D -m 644 etc/phonehome/phonehome.conf $(DESTDIR)/etc/phonehome/phonehome.conf

install-systemd: install
	install -D -m 644 scripts/systemd/phonehome-keygen.service $(DESTDIR)/etc/systemd/system/phonehome-keygen.service
	install -D -m 644 scripts/systemd/phonehome-register.service $(DESTDIR)/etc/systemd/system/phonehome-register.service

install-initd: install
	install -D -m 755 scripts/initd/phonehome-keygen $(DESTDIR)/etc/init.d/phonehome-keygen
	install -D -m 755 scripts/initd/phonehome-register $(DESTDIR)/etc/init.d/phonehome-register

clean:
	rm -f $(SERVER_TARGET) $(TEST_TARGET) $(TEST_SERVER) $(TEST_PH_TARGET)
