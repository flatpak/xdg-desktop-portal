libexec_PROGRAMS += \
	xdg-desktop-portal \
	$(NULL)

service_in_files += src/org.freedesktop.portal.Desktop.service.in
dbus_service_DATA += src/org.freedesktop.portal.Desktop.service
DISTCLEANFILES += src/org.freedesktop.portal.Desktop.service

xdp_dbus_built_sources = src/xdp-dbus.c src/xdp-dbus.h
BUILT_SOURCES += $(xdp_dbus_built_sources)

$(xdp_dbus_built_sources) : $(FLATPAK_INTERFACES_DIR)/org.freedesktop.portal.Documents.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.portal.      \
		--c-namespace XdpDbus                           \
		--generate-c-code $(builddir)/src/xdp-dbus      \
		--annotate "org.freedesktop.portal.Documents.Add()" "org.gtk.GDBus.C.UnixFD" "true" \
		--annotate "org.freedesktop.portal.Documents.AddNamed()" "org.gtk.GDBus.C.UnixFD" "true" \
		$(FLATPAK_INTERFACES_DIR)/org.freedesktop.portal.Documents.xml     \
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
	$(NULL)

xdg_desktop_portal_SOURCES = \
	src/xdg-desktop-portal.c	\
	src/flatpak-utils.c	\
	src/flatpak-utils.h	\
	src/flatpak-portal-error.c	\
	src/flatpak-portal-error.h	\
	src/xdg-desktop-resources.c	\
	$(NULL)

xdg_desktop_portal_LDADD = $(BASE_LIBS)
xdg_desktop_portal_CFLAGS = \
	-DPKGDATADIR=\"$(pkgdatadir)\" $(BASE_CFLAGS) \
	-I$(srcdir)/src -I$(builddir)/src \
	$(NULL)
