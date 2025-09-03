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

SRCS := src/olsrd_status_plugin.c src/httpd.c src/util.c src/connections.c
HDRS := src/httpd.h src/util.h

CFLAGS   ?= -O2
CFLAGS   += -fPIC
CPPFLAGS += -D_GNU_SOURCE -I$(OLSRD_INC) -I$(OLSRD_SRC) -Isrc
WARNFLAGS?= -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes -Wmissing-prototypes
LDFLAGS  += -shared
LDLIBS   += -lpthread

PREFIX   ?= /usr
LIBDIR   ?= $(PREFIX)/lib/olsrd
SHAREDIR ?= $(PREFIX)/share/olsrd-status-plugin
ASSETDIR ?= $(SHAREDIR)/www

SCRIPTS := scripts/fetch-assets.sh

.PHONY: all clean status_plugin status_plugin_clean install uninstall status_plugin_install status_plugin_uninstall

all: status_plugin

$(BUILDDIR)/$(SONAME): $(SRCS) $(HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(WARNFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

$(BUILDDIR):
	$(MKDIR_P) $(BUILDDIR)

clean: status_plugin_clean
status_plugin_clean:
	$(RM) -r $(BUILDDIR)

status_plugin: $(BUILDDIR)/$(SONAME)

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
