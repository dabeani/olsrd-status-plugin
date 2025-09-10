# olsrd-status-plugin — standalone Makefile (v10)
PLUGIN_NAME := olsrd_status
PLUGIN_VER  := 1.0
SONAME      := $(PLUGIN_NAME).so.$(PLUGIN_VER)
BUILDDIR    := build

OLSRD_TOP ?= ../..
OLSRD_INC ?= $(OLSRD_TOP)/include
OLSRD_SRC ?= $(OLSRD_TOP)/src

CC      ?= cc
RM      ?= rm -f
MKDIR_P ?= mkdir -p

SRCS := src/olsrd_status_plugin.c src/httpd.c src/util.c src/connections.c rev/discover/ubnt_discover.c
HDRS := src/httpd.h src/util.h rev/discover/ubnt_discover.h

CFLAGS   ?= -O2
CFLAGS   += -fPIC
CPPFLAGS += -D_GNU_SOURCE -I$(OLSRD_INC) -I$(OLSRD_SRC) -Isrc -Irev/discover
WARNFLAGS?= -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes
LDFLAGS  += -shared
LDLIBS   += -lpthread

# Try to detect libcurl via pkg-config. If present, add its cflags/libs
PKG_CONFIG ?= pkg-config
CURL_DETECTED := $(shell $(PKG_CONFIG) --exists libcurl 2>/dev/null && echo 1 || echo 0)
ifeq ($(CURL_DETECTED),1)
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl)
CPPFLAGS += $(CURL_CFLAGS) -DHAVE_LIBCURL=1
LDLIBS += $(CURL_LIBS)
$(info >>> libcurl detected via pkg-config; building with libcurl support)
else
$(info >>> libcurl NOT detected; building without libcurl support)
endif

# Control whether the plugin will spawn an external `curl` binary as a fallback
# Set ENABLE_CURL_FALLBACK=0 to disable the external-curl fallback and require
# either libcurl or no remote HTTPS fetching.
ENABLE_CURL_FALLBACK ?= 1
ifeq ($(ENABLE_CURL_FALLBACK),0)
CPPFLAGS += -DNO_CURL_FALLBACK=1
$(info >>> external curl fallback: DISABLED (set ENABLE_CURL_FALLBACK=1 to enable))
else
$(info >>> external curl fallback: ENABLED (set ENABLE_CURL_FALLBACK=0 to disable))
endif

# At build time we can optionally require that at least one fetch path is
# available (libcurl or curl binary). Set REQUIRE_FETCH=1 to make the build
# fail when neither is available.
REQUIRE_FETCH ?= 0
CURL_BIN_EXISTS := $(shell command -v curl >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(REQUIRE_FETCH),1)
ifeq ($(CURL_DETECTED),0)
ifeq ($(CURL_BIN_EXISTS),0)
$(error >>> REQUIRE_FETCH=1: neither libcurl (dev) detected nor external 'curl' binary present; aborting build)
else
$(info >>> REQUIRE_FETCH=1: libcurl not detected but external curl binary found)
endif
else
$(info >>> REQUIRE_FETCH=1: libcurl detected; OK)
endif
endif

PREFIX   ?= /usr
LIBDIR   ?= $(PREFIX)/lib/olsrd
SHAREDIR ?= $(PREFIX)/share/olsrd-status-plugin
ASSETDIR ?= $(SHAREDIR)/www

SCRIPTS := scripts/fetch-assets.sh scripts/debug-plugin.sh

.PHONY: all clean status_plugin status_plugin_clean install uninstall status_plugin_install status_plugin_uninstall

CLI_BIN_DIR := bin
CLI_BIN := $(CLI_BIN_DIR)/ubnt-discover
.PHONY: ubnt_discover_cli

all: status_plugin ubnt_discover_cli

$(BUILDDIR)/$(SONAME): $(SRCS) $(HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(WARNFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

$(BUILDDIR):
	$(MKDIR_P) $(BUILDDIR)

clean: status_plugin_clean
status_plugin_clean:
	$(RM) -r $(BUILDDIR)

status_plugin: $(BUILDDIR)/$(SONAME)

ubnt_discover_cli: $(CLI_BIN)

$(CLI_BIN): rev/discover/ubnt_discover.c rev/discover/ubnt_discover.h | $(CLI_BIN_DIR)
	@if [ -f rev/discover/ubnt_discover_cli.c ]; then \
		$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ rev/discover/ubnt_discover_cli.c rev/discover/ubnt_discover.c -lpthread; \
	else \
		$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ rev/discover/ubnt_discover.c -lpthread; \
	fi

$(CLI_BIN_DIR):
	$(MKDIR_P) $(CLI_BIN_DIR)

install: status_plugin_install
uninstall: status_plugin_uninstall

status_plugin_install: status_plugin
	@echo ">>> Installing plugin to $(DESTDIR)$(LIBDIR)"
	$(MKDIR_P) "$(DESTDIR)$(LIBDIR)"
	install -m 0755 "$(BUILDDIR)/$(SONAME)" "$(DESTDIR)$(LIBDIR)/"
	@echo ">>> Installing shared data to $(DESTDIR)$(SHAREDIR)"
	$(MKDIR_P) "$(DESTDIR)$(ASSETDIR)"
	@if [ -d www ]; then \
	  echo ">>> Copying local www/ assets into $(DESTDIR)$(ASSETDIR)"; \
	  cp -a www/. "$(DESTDIR)$(ASSETDIR)/"; \
	else \
	  echo ">>> Note: no local www/ dir found; you can populate later with fetch-assets.sh"; \
	fi
	@for s in $(SCRIPTS); do \
	  if [ -f $$s ]; then \
	    echo ">>> Installing script $$s -> $(DESTDIR)$(SHAREDIR)"; \
	    install -m 0755 $$s "$(DESTDIR)$(SHAREDIR)/"; \
	  fi; \
	done
	@echo ">>> Done."

status_plugin_uninstall:
	@echo ">>> Removing plugin and data from $(DESTDIR)$(PREFIX)"
	$(RM) "$(DESTDIR)$(LIBDIR)/$(SONAME)"
	$(RM) -r "$(DESTDIR)$(SHAREDIR)"
	@echo ">>> Done."
