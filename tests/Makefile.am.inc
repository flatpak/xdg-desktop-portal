TESTS_ENVIRONMENT = FLATPAK_TESTS_DEBUG=1 \
	FLATPAK_TRIGGERSDIR=$$(cd $(top_srcdir) && pwd)/triggers \
	FLATPAK_DBUSPROXY=$$(cd $(top_builddir) && pwd)/flatpak-dbus-proxy \
	GI_TYPELIB_PATH=$$(cd $(top_builddir) && pwd)$${GI_TYPELIB_PATH:+:$$GI_TYPELIB_PATH} \
	LD_LIBRARY_PATH=$$(cd $(top_builddir)/.libs && pwd)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	PATH=$$(cd $(top_builddir) && pwd):$${PATH} \
	$(NULL)

testdb_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) -I$(srcdir)/document-portal -I$(builddir)/document-portal -I$(builddir)/
testdb_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(DB_SOURCES) \
	$(NULL)
testdb_SOURCES = tests/testdb.c

test_doc_portal_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
test_doc_portal_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(NULL)
test_doc_portal_SOURCES = tests/test-doc-portal.c
nodist_test_doc_portal_SOURCES = document-portal/document-portal-dbus.c

EXTRA_test_doc_portal_DEPENDENCIES = tests/services/org.freedesktop.impl.portal.PermissionStore.service tests/services/org.freedesktop.portal.Documents.service

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

if ENABLE_INSTALLED_TESTS
dist_installed_test_dbs_DATA = tests/dbs/no_tables
endif

dist_test_scripts = \
	$(NULL)

test_programs = testdb test-doc-portal

DISTCLEANFILES += \
	tests/services/org.freedesktop.portal.Documents.service \
	tests/services/org.freedesktop.impl.portal.PermissionStore.service \
	$(NULL)
