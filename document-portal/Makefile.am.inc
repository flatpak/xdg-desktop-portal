libexec_PROGRAMS += \
	xdg-permission-store \
	xdg-document-portal \
	$(NULL)

service_in_files += \
	document-portal/xdg-permission-store.service.in \
	document-portal/xdg-document-portal.service.in \
	$(NULL)

systemduserunit_DATA += \
	document-portal/xdg-permission-store.service \
	document-portal/xdg-document-portal.service \
	$(NULL)

service_in_files += \
	document-portal/org.freedesktop.impl.portal.PermissionStore.service.in \
	document-portal/org.freedesktop.portal.Documents.service.in \
	$(NULL)

dbus_service_DATA += \
	document-portal/org.freedesktop.impl.portal.PermissionStore.service \
	document-portal/org.freedesktop.portal.Documents.service \
	$(NULL)

nodist_xdg_permission_store_SOURCES = \
	document-portal/permission-store-dbus.c \
	document-portal/permission-store-dbus.h \
	$(NULL)

BUILT_SOURCES += $(nodist_xdg_permission_store_SOURCES)
CLEANFILES += $(nodist_xdg_permission_store_SOURCES)

document-portal/permission-store-dbus.c: data/org.freedesktop.impl.portal.PermissionStore.xml Makefile
	mkdir -p $(builddir)/document-portal
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.impl.portal.	\
		--c-namespace Xdg				\
		--generate-c-code $(builddir)/document-portal/permission-store-dbus	\
		$(srcdir)/data/org.freedesktop.impl.portal.PermissionStore.xml	\
		$(NULL)

document-portal/document-portal-dbus.c: data/org.freedesktop.portal.Documents.xml data/org.freedesktop.portal.FileTransfer.xml Makefile
	mkdir -p $(builddir)/document-portal
	$(AM_V_GEN) $(GDBUS_CODEGEN)				\
		--interface-prefix org.freedesktop.portal.	\
		--c-namespace XdpDbus				\
		--generate-c-code $(builddir)/document-portal/document-portal-dbus	\
		$(srcdir)/data/org.freedesktop.portal.Documents.xml	\
		$(srcdir)/data/org.freedesktop.portal.FileTransfer.xml	\
		$(NULL)

document-portal/%-dbus.h: document-portal/%-dbus.c
	@true # Built as a side-effect of the rules for the .c

DB_SOURCES = \
	document-portal/permission-db.c	\
	document-portal/permission-db.h	\
	document-portal/gvdb/gvdb-reader.h	\
	document-portal/gvdb/gvdb-format.h	\
	document-portal/gvdb/gvdb-reader.c	\
	document-portal/gvdb/gvdb-builder.h	\
	document-portal/gvdb/gvdb-builder.c	\
	$(NULL)


xdg_permission_store_SOURCES = \
	src/xdp-utils.c	\
	src/xdp-utils.h	\
	document-portal/permission-store.c	\
	document-portal/xdg-permission-store.c	\
	document-portal/xdg-permission-store.h	\
	$(DB_SOURCES) \
	$(NULL)

xdg_permission_store_LDADD = $(AM_LDADD) $(BASE_LIBS)
xdg_permission_store_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) -I$(srcdir)/document-portal -I$(builddir)/document-portal

nodist_xdg_document_portal_SOURCES = \
	$(nodist_xdg_permission_store_SOURCES) \
	document-portal/document-portal-dbus.c \
	document-portal/document-portal-dbus.h \
	$(NULL)

BUILT_SOURCES += $(nodist_xdg_document_portal_SOURCES)
CLEANFILES += $(nodist_xdg_document_portal_SOURCES)

xdg_document_portal_SOURCES = \
	src/xdp-utils.c	\
	src/xdp-utils.h	\
	document-portal/document-portal.h		\
	document-portal/document-portal.c		\
	document-portal/file-transfer.h			\
	document-portal/file-transfer.c			\
	document-portal/document-enums.h		\
	document-portal/document-store.h		\
	document-portal/document-store.c		\
	document-portal/document-portal-fuse.h		\
	document-portal/document-portal-fuse.c		\
	$(DB_SOURCES) \
	$(NULL)

xdg_document_portal_LDADD = $(AM_LDADD) $(BASE_LIBS) $(FUSE3_LIBS)
xdg_document_portal_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)  $(FUSE3_CFLAGS) -I$(srcdir)/document-portal -I$(builddir)/document-portal
