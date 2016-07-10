libexec_PROGRAMS += \
	xdg-desktop-portal \
	$(NULL)

service_in_files += src/org.freedesktop.portal.Desktop.service.in
dbus_service_DATA += src/org.freedesktop.portal.Desktop.service
DISTCLEANFILES += src/org.freedesktop.portal.Desktop.service

xdp_dbus_built_sources = src/xdp-dbus.c src/xdp-dbus.h
xdp_impl_dbus_built_sources = src/xdp-impl-dbus.c src/xdp-impl-dbus.h
BUILT_SOURCES += $(xdp_dbus_built_sources) $(xdp_impl_dbus_built_sources)

FLATPAK_IFACE_FILES =\
	$(FLATPAK_INTERFACES_DIR)/org.freedesktop.portal.Documents.xml \
	$(NULL)

FLATPAK_IMPL_IFACE_FILES =\
	$(FLATPAK_INTERFACES_DIR)/org.freedesktop.impl.portal.PermissionStore.xml \
	$(NULL)

PORTAL_IFACE_FILES =\
	data/org.freedesktop.portal.Request.xml \
	data/org.freedesktop.portal.FileChooser.xml \
	data/org.freedesktop.portal.OpenURI.xml \
	data/org.freedesktop.portal.Print.xml \
	data/org.freedesktop.portal.NetworkMonitor.xml \
	data/org.freedesktop.portal.ProxyResolver.xml \
	data/org.freedesktop.portal.Screenshot.xml \
	data/org.freedesktop.portal.Notification.xml \
	data/org.freedesktop.portal.Inhibit.xml \
	$(NULL)

PORTAL_IMPL_IFACE_FILES =\
	data/org.freedesktop.impl.portal.Request.xml \
	data/org.freedesktop.impl.portal.FileChooser.xml \
	data/org.freedesktop.impl.portal.AppChooser.xml \
	data/org.freedesktop.impl.portal.Print.xml \
	data/org.freedesktop.impl.portal.Screenshot.xml \
	data/org.freedesktop.impl.portal.Notification.xml \
	data/org.freedesktop.impl.portal.Inhibit.xml \
	$(NULL)

$(xdp_dbus_built_sources) : $(FLATPAK_IFACE_FILES) $(PORTAL_IFACE_FILES)
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.portal.      \
		--c-namespace Xdp                               \
		--generate-c-code $(builddir)/src/xdp-dbus      \
		--annotate "org.freedesktop.portal.Documents.Add()" "org.gtk.GDBus.C.UnixFD" "true" \
		--annotate "org.freedesktop.portal.Documents.AddNamed()" "org.gtk.GDBus.C.UnixFD" "true" \
		--annotate "org.freedesktop.portal.Print.Print()" "org.gtk.GDBus.C.UnixFD" "true" \
		$^ \
		$(NULL)

$(xdp_impl_dbus_built_sources) : $(FLATPAK_IMPL_IFACE_FILES) $(PORTAL_IMPL_IFACE_FILES)
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.impl.portal. \
		--c-namespace XdpImpl                           \
		--generate-c-code $(builddir)/src/xdp-impl-dbus \
		--annotate "org.freedesktop.impl.portal.Print.Print()" "org.gtk.GDBus.C.UnixFD" "true" \
		$^ \
		$(NULL)

xdg_desktop_resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/src/xdg-desktop-portal.gresource.xml)

src/xdg-desktop-resources.h: src/xdg-desktop-portal.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-header

src/xdg-desktop-resources.c: src/xdg-desktop-portal.gresource.xml $(xdg_desktop_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-source

EXTRA_DIST += src/xdg-desktop-portal.gresource.xml

DISTCLEANFILES += \
	$(xdp_dbus_built_sources)		\
	$(xdp_impl_dbus_built_sources)		\
	$(NULL)

nodist_xdg_desktop_portal_SOURCES = \
	$(xdp_dbus_built_sources)		\
	$(xdp_impl_dbus_built_sources)		\
	$(NULL)

xdg_desktop_portal_SOURCES = \
	src/xdg-desktop-portal.c	\
	src/file-chooser.c		\
	src/file-chooser.h		\
	src/open-uri.c			\
	src/open-uri.h			\
	src/print.c			\
	src/print.h			\
	src/network-monitor.c		\
	src/network-monitor.h		\
	src/proxy-resolver.c		\
	src/proxy-resolver.h		\
	src/screenshot.c		\
	src/screenshot.h		\
        src/notification.c              \
        src/notification.h              \
        src/inhibit.c                   \
        src/inhibit.h                   \
	src/request.c			\
	src/request.h			\
        src/documents.c                 \
        src/documents.h                 \
        src/permissions.c               \
        src/permissions.h               \
	src/xdp-utils.c			\
	src/xdp-utils.h			\
	src/xdg-desktop-resources.c	\
	$(NULL)

xdg_desktop_portal_LDADD = $(BASE_LIBS)
xdg_desktop_portal_CFLAGS = \
	-DPKGDATADIR=\"$(pkgdatadir)\" $(BASE_CFLAGS) \
	-I$(srcdir)/src -I$(builddir)/src \
	$(NULL)
xdg_desktop_portal_CPPFLAGS = \
	-DGETTEXT_PACKAGE=\"$(GETTEXT_PACKAGE)\"        \
	-DLOCALEDIR=\"$(localedir)\"			\
	$(NULL)
