libexec_PROGRAMS += \
	xdg-desktop-portal \
	$(NULL)

service_in_files += src/org.freedesktop.portal.Desktop.service.in
dbus_service_DATA += src/org.freedesktop.portal.Desktop.service
DISTCLEANFILES += src/org.freedesktop.portal.Desktop.service

xdp_dbus_built_sources = src/xdp-dbus.c src/xdp-dbus.h
xdp_impl_dbus_built_sources = src/xdp-impl-dbus.c src/xdp-impl-dbus.h
BUILT_SOURCES += $(xdp_dbus_built_sources) $(xdp_impl_dbus_built_sources)

PORTAL_IFACE_FILES =\
	data/org.freedesktop.portal.Request.xml \
	data/org.freedesktop.portal.FileChooser.xml \
	data/org.freedesktop.portal.OpenURI.xml \
	data/org.freedesktop.portal.Print.xml \
	data/org.freedesktop.portal.NetworkMonitor.xml \
	$(NULL)

PORTAL_IMPL_IFACE_FILES =\
	data/org.freedesktop.impl.portal.FileChooser.xml \
	data/org.freedesktop.impl.portal.AppChooser.xml \
	data/org.freedesktop.impl.portal.Print.xml \
	$(NULL)

$(xdp_dbus_built_sources) : $(FLATPAK_INTERFACES_DIR)/org.freedesktop.portal.Documents.xml $(PORTAL_IFACE_FILES)
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.portal.      \
		--c-namespace Xdp                               \
		--generate-c-code $(builddir)/src/xdp-dbus      \
		--annotate "org.freedesktop.portal.Documents.Add()" "org.gtk.GDBus.C.UnixFD" "true" \
		--annotate "org.freedesktop.portal.Documents.AddNamed()" "org.gtk.GDBus.C.UnixFD" "true" \
		$(FLATPAK_INTERFACES_DIR)/org.freedesktop.portal.Documents.xml     \
		$(PORTAL_IFACE_FILES) \
		$(NULL)

$(xdp_impl_dbus_built_sources) : $(PORTAL_IMPL_IFACE_FILES)
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.impl.portal. \
		--c-namespace XdpImpl                           \
		--generate-c-code $(builddir)/src/xdp-impl-dbus \
		$(PORTAL_IMPL_IFACE_FILES) \
		$(NULL)

xdg_desktop_resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/src/xdg-desktop-portal.gresource.xml)

src/xdg-desktop-resources.h: src/xdg-desktop-portal.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-header

src/xdg-desktop-resources.c: src/xdg-desktop-portal.gresource.xml $(xdg_desktop_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-source

EXTRA_DIST += src/xdg-desktop-portal.gresource.xml

nodist_xdg_desktop_portal_SOURCES = \
	$(xdp_dbus_built_sources)		\
	$(xdp_impl_dbus_built_sources)		\
	$(NULL)

xdg_desktop_portal_SOURCES = \
	src/xdg-desktop-portal.c	\
	src/file-chooser.c              \
	src/file-chooser.h              \
        src/open-uri.c                  \
        src/open-uri.h                  \
        src/print.c                     \
        src/print.h                     \
        src/network-monitor.c           \
        src/network-monitor.h           \
	src/request.c                   \
	src/request.h                   \
	src/xdp-utils.c             \
	src/xdp-utils.h             \
	src/xdg-desktop-resources.c	\
	$(NULL)

xdg_desktop_portal_LDADD = $(BASE_LIBS)
xdg_desktop_portal_CFLAGS = \
	-DPKGDATADIR=\"$(pkgdatadir)\" $(BASE_CFLAGS) \
	-I$(srcdir)/src -I$(builddir)/src \
	$(NULL)
