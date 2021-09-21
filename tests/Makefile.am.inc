test_programs =

TESTS_ENVIRONMENT = \
	PATH=$$(cd $(top_builddir) && pwd):$${PATH} \
	XDG_DATA_DIRS=$(top_srcdir)/tests/share/:$${XDG_DATA_DIRS} \
	XDP_UNINSTALLED=1 \
	$(NULL)

testdb_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) -I$(srcdir)/document-portal -I$(builddir)/document-portal -I$(builddir)/
testdb_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(NULL)
testdb_SOURCES = tests/testdb.c	$(DB_SOURCES)

test_doc_portal_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(FUSE3_CFLAGS)
test_doc_portal_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(FUSE3_LIBS) \
	$(NULL)
test_doc_portal_SOURCES = \
	tests/can-use-fuse.c \
	tests/can-use-fuse.h \
	tests/test-doc-portal.c \
	tests/utils.c \
	tests/utils.h \
	$(NULL)
nodist_test_doc_portal_SOURCES = document-portal/document-portal-dbus.c

EXTRA_test_doc_portal_DEPENDENCIES = tests/services/org.freedesktop.impl.portal.PermissionStore.service tests/services/org.freedesktop.portal.Documents.service

test_portals_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) $(LIBPORTAL_CFLAGS)
test_portals_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(LIBPORTAL_LIBS) \
	$(NULL)
test_portals_SOURCES = tests/test-portals.c
test_portals_CPPFLAGS = $(AM_CPPFLAGS) -DLIBEXECDIR=\"$(libexecdir)\"
if HAVE_LIBPORTAL
test_portals_CFLAGS += $(LIBPORTAL_CFLAGS)
test_portals_LDADD += $(LIBPORTAL_LIBS)
test_portals_SOURCES += \
	tests/account.c \
	tests/account.h \
	tests/background.c \
	tests/background.h \
        tests/camera.c \
        tests/camera.h \
	tests/email.c \
	tests/email.h \
	tests/filechooser.c \
	tests/filechooser.h \
        tests/inhibit.c \
        tests/inhibit.h \
        tests/location.c \
        tests/location.h \
        tests/notification.c \
        tests/notification.h \
        tests/openuri.c \
        tests/openuri.h \
	tests/print.c \
	tests/print.h \
	tests/screenshot.c \
	tests/screenshot.h \
	tests/trash.c \
	tests/trash.h \
	tests/wallpaper.c \
	tests/wallpaper.h \
        tests/glib-backports.c \
        tests/glib-backports.h \
	$(NULL)
endif
nodist_test_portals_SOURCES = \
	src/xdp-dbus.c \
	src/xdp-impl-dbus.c \
	src/xdp-utils.c \
        document-portal/permission-store-dbus.c \
		tests/utils.c \
		tests/utils.h \
	$(NULL)
test_programs += test-portals

test_permission_store_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
test_permission_store_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(NULL)
test_permission_store_SOURCES = tests/test-permission-store.c
nodist_test_permission_store_SOURCES = \
	document-portal/permission-store-dbus.c src/xdp-utils.c \
	tests/utils.c \
	tests/utils.h \
	$(NULL)

EXTRA_test_permission_store_DEPENDENCIES = tests/services/org.freedesktop.impl.portal.PermissionStore.service tests/services/org.freedesktop.portal.Documents.service

test_programs += test-xdp-utils
test_xdp_utils_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
test_xdp_utils_LDADD = $(AM_LD_ADD) $(BASE_LIBS)
test_xdp_utils_SOURCES = tests/test-xdp-utils.c src/xdp-utils.c


tests/services/org.freedesktop.portal.Documents.service: document-portal/org.freedesktop.portal.Documents.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

tests/services/org.freedesktop.impl.portal.PermissionStore.service: document-portal/org.freedesktop.impl.portal.PermissionStore.service.in
	mkdir -p tests/services
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(abs_top_builddir)|" $< > $@

install-test-data-hook:
if ENABLE_INSTALLED_TESTS
	mkdir -p $(DESTDIR)$(installed_testdir)/services
	ln -sf $(dbus_servicedir)/org.freedesktop.portal.Documents.service $(DESTDIR)$(installed_testdir)/services/
	ln -sf $(dbus_servicedir)/org.freedesktop.impl.portal.PermissionStore.service $(DESTDIR)$(installed_testdir)/services/
endif

installed_test_dbsdir = $(installed_testdir)/dbs
installed_test_portalsdir = $(installed_testdir)/portals

if ENABLE_INSTALLED_TESTS
dist_installed_test_dbs_DATA = tests/dbs/no_tables
dist_installed_test_portals_DATA = tests/portals/test.portal
endif

dist_installed_test_DATA = \
	tests/session.conf.in \
	$(NULL)

dist_installed_test_extra_scripts = \
	tests/test-document-fuse.py \
	$(NULL)

dist_test_scripts = \
	tests/test-document-fuse.sh \
	$(NULL)

dist_test_apps = \
	tests/share/applications/furrfix.desktop \
	tests/share/applications/mimeinfo.cache \
	$(NULL)

test_programs += \
	testdb \
	test-doc-portal \
	test-permission-store \
	$(NULL)

DISTCLEANFILES += \
	tests/services/org.freedesktop.portal.Documents.service \
	tests/services/org.freedesktop.impl.portal.PermissionStore.service \
        session.conf \
        dbus-session-bus-address \
        dbus-session-bus-pid \
	$(NULL)

EXTRA_DIST += \
	tests/share/applications/furrfix.desktop \
	tests/share/applications/mimeinfo.cache \
	$(NULL)
