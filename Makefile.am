NULL =

bin_PROGRAMS = $(NULL)
noinst_PROGRAMS = $(NULL)
noinst_LTLIBRARIES = $(NULL)
noinst_SCRIPTS =
noinst_DATA =
libexec_PROGRAMS = $(NULL)
test_extra_programs =
CLEANFILES = $(NULL)
DISTCLEANFILES= $(NULL)
BUILT_SOURCES = $(NULL)
EXTRA_DIST = tap-driver.sh tap-test

include $(top_srcdir)/glib-tap.mk
include $(top_srcdir)/Makefile.am.inc

SUBDIRS = doc po

%.service: %.service.in
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" -e "s|\@extraargs\@||" $< > $@

dbus_servicedir = $(DBUS_SERVICE_DIR)
service_in_files = $(NULL)
dbus_service_DATA = $(NULL)
systemduserunit_DATA = $(NULL)
CLEANFILES += $(dbus_service_DATA)
CLEANFILES += $(systemduserunit_DATA)
EXTRA_DIST += $(service_in_files)

ACLOCAL_AMFLAGS = --install -I m4 ${ACLOCAL_FLAGS}
AM_CPPFLAGS =							\
	-DG_LOG_DOMAIN=\"xdg-desktop-portal\"			\
	$(NULL)

lib_LTLIBRARIES =

include data/Makefile.am.inc
include document-portal/Makefile.am.inc
include src/Makefile.am.inc
include tests/backend/Makefile.am.inc
include tests/Makefile.am.inc

pkgconfigdir = $(datadir)/pkgconfig
pkgconfig_DATA = xdg-desktop-portal.pc
EXTRA_DIST += xdg-desktop-portal.pc.in

EXTRA_DIST += README.md

install-data-hook:
	$(MAKE) $(AM_MAKEFLAGS) install-test-data-hook

AM_DISTCHECK_CONFIGURE_FLAGS =


.PHONY: coverage lcov-clean genlcov

coverage:
	$(AM_V_GEN) $(MAKE) $(AM_MAKEFLAGS) lcov-clean
	$(AM_V_GEN) $(MAKE) -j $(getconf _NPROCESSORS_ONLN) check
	$(AM_V_GEN) $(MAKE) $(AM_MAKEFLAGS) genlcov

lcov-clean:
	$(AM_V_GEN) $(LCOV) --directory $(top_builddir) --zerocounters

genlcov:
	$(AM_V_GEN) $(LCOV) --directory $(top_builddir) --capture --output-file coverage.info
	$(AM_V_GEN) $(GENHTML) --prefix $(top_builddir) --output-directory coverage coverage.info
