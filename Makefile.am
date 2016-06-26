NULL =

bin_PROGRAMS = $(NULL)
noinst_PROGRAMS = $(NULL)
noinst_LTLIBRARIES = $(NULL)
libexec_PROGRAMS = $(NULL)
CLEANFILES = $(NULL)
DISTCLEANFILES= $(NULL)
BUILT_SOURCES = $(NULL)
EXTRA_DIST =

SUBDIRS = doc

%.service: %.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" -e "s|\@extraargs\@||" $< > $@

dbus_servicedir = $(DBUS_SERVICE_DIR)
service_in_files = $(NULL)
dbus_service_DATA = $(NULL)
CLEANFILES += $(dbus_service_DATA)
EXTRA_DIST += $(service_in_files)

AM_CPPFLAGS =							\
	-DG_LOG_DOMAIN=\"xdg-desktop-portal\"			\
	$(NULL)

lib_LTLIBRARIES =

include data/Makefile.am.inc
include src/Makefile.am.inc

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = xdg-desktop-portal.pc
EXTRA_DIST += xdg-desktop-portal.pc.in

EXTRA_DIST += README.md

AM_DISTCHECK_CONFIGURE_FLAGS =
