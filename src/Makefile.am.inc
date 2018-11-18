libexec_PROGRAMS += \
	xdg-desktop-portal \
	xdg-desktop-portal-validate-icon \
	$(NULL)

service_in_files += \
	src/org.freedesktop.portal.Desktop.service.in \
	src/xdg-desktop-portal.service.in \
	${NULL}

dbus_service_DATA += src/org.freedesktop.portal.Desktop.service
systemduserunit_DATA += src/xdg-desktop-portal.service

xdp_dbus_built_sources = src/xdp-dbus.c src/xdp-dbus.h
xdp_impl_dbus_built_sources = src/xdp-impl-dbus.c src/xdp-impl-dbus.h
geoclue_built_sources = src/geoclue-dbus.c src/geoclue-dbus.h
BUILT_SOURCES += $(xdp_dbus_built_sources) $(xdp_impl_dbus_built_sources) $(geoclue_built_sources)

PORTAL_IFACE_FILES =\
	data/org.freedesktop.portal.Documents.xml \
	data/org.freedesktop.portal.Request.xml \
	data/org.freedesktop.portal.Session.xml \
	data/org.freedesktop.portal.FileChooser.xml \
	data/org.freedesktop.portal.Trash.xml \
	data/org.freedesktop.portal.OpenURI.xml \
	data/org.freedesktop.portal.Print.xml \
	data/org.freedesktop.portal.NetworkMonitor.xml \
	data/org.freedesktop.portal.ProxyResolver.xml \
	data/org.freedesktop.portal.Screenshot.xml \
	data/org.freedesktop.portal.Notification.xml \
	data/org.freedesktop.portal.Inhibit.xml \
	data/org.freedesktop.portal.Device.xml \
	data/org.freedesktop.portal.Account.xml \
	data/org.freedesktop.portal.Email.xml \
	data/org.freedesktop.portal.ScreenCast.xml \
	data/org.freedesktop.portal.RemoteDesktop.xml \
	data/org.freedesktop.portal.Location.xml \
	data/org.freedesktop.portal.Settings.xml \
	$(NULL)

PORTAL_IMPL_IFACE_FILES =\
	data/org.freedesktop.impl.portal.PermissionStore.xml \
	data/org.freedesktop.impl.portal.Request.xml \
	data/org.freedesktop.impl.portal.Session.xml \
	data/org.freedesktop.impl.portal.FileChooser.xml \
	data/org.freedesktop.impl.portal.AppChooser.xml \
	data/org.freedesktop.impl.portal.Print.xml \
	data/org.freedesktop.impl.portal.Screenshot.xml \
	data/org.freedesktop.impl.portal.Notification.xml \
	data/org.freedesktop.impl.portal.Inhibit.xml \
	data/org.freedesktop.impl.portal.Access.xml \
	data/org.freedesktop.impl.portal.Account.xml \
	data/org.freedesktop.impl.portal.Email.xml \
	data/org.freedesktop.impl.portal.ScreenCast.xml \
	data/org.freedesktop.impl.portal.RemoteDesktop.xml \
	data/org.freedesktop.impl.portal.Settings.xml \
	$(NULL)

$(xdp_dbus_built_sources) : $(PORTAL_IFACE_FILES)
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.portal.      \
		--c-namespace Xdp                               \
		--generate-c-code $(builddir)/src/xdp-dbus      \
		$^ \
		$(NULL)

$(xdp_impl_dbus_built_sources) : $(PORTAL_IMPL_IFACE_FILES)
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.impl.portal. \
		--c-namespace XdpImpl                           \
		--generate-c-code $(builddir)/src/xdp-impl-dbus \
		$^ \
		$(NULL)

$(geoclue_built_sources) : src/org.freedesktop.GeoClue2.Client.xml
	$(AM_V_GEN) $(GDBUS_CODEGEN)                            \
		--interface-prefix org.freedesktop.GeoClue2. \
		--c-namespace Geoclue                        \
		--generate-c-code $(builddir)/src/geoclue-dbus \
		$^ \
		$(NULL)

EXTRA_DIST += src/org.freedesktop.GeoClue2.Client.xml

xdg_desktop_resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(srcdir) --generate-dependencies $(srcdir)/src/xdg-desktop-portal.gresource.xml)

src/xdg-desktop-resources.h: src/xdg-desktop-portal.gresource.xml
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-header

src/xdg-desktop-resources.c: src/xdg-desktop-portal.gresource.xml $(xdg_desktop_resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(srcdir) --c-name _xdg_desktop --generate-source

EXTRA_DIST += src/xdg-desktop-portal.gresource.xml

CLEANFILES += \
	$(nodist_xdg_desktop_portal_SOURCES) \
	$(NULL)

nodist_xdg_desktop_portal_SOURCES = \
	$(xdp_dbus_built_sources)		\
	$(xdp_impl_dbus_built_sources)		\
	$(geoclue_built_sources)		\
	src/xdg-desktop-resources.c		\
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
        src/device.c                    \
        src/device.h                    \
        src/account.c                   \
        src/account.h                   \
	src/request.c			\
	src/request.h			\
	src/call.c			\
	src/call.h			\
        src/documents.c                 \
        src/documents.h                 \
        src/permissions.c               \
        src/permissions.h               \
        src/email.c                     \
        src/email.h                     \
	src/settings.c			\
	src/settings.h			\
	src/session.c			\
	src/session.h			\
	src/trash.c			\
	src/trash.h			\
	src/xdp-utils.c			\
	src/xdp-utils.h			\
	src/fc-monitor.c		\
	src/fc-monitor.h		\
	$(NULL)

if HAVE_PIPEWIRE
xdg_desktop_portal_SOURCES += \
	src/screen-cast.c		\
	src/screen-cast.h		\
	src/remote-desktop.c		\
	src/remote-desktop.h		\
	$(NULL)
endif

if HAVE_GEOCLUE
xdg_desktop_portal_SOURCES += \
	src/location.c                  \
	src/location.h                  \
	$(NULL)
endif

xdg_desktop_portal_LDADD = $(BASE_LIBS) $(PIPEWIRE_LIBS) $(GEOCLUE_LIBS)
xdg_desktop_portal_CFLAGS = \
	-DPKGDATADIR=\"$(pkgdatadir)\" \
	-DLIBEXECDIR=\"$(libexecdir)\" \
	$(BASE_CFLAGS) $(PIPEWIRE_CFLAGS) $(GEOCLUE_CFLAGS) \
	-I$(srcdir)/src -I$(builddir)/src \
	$(NULL)
xdg_desktop_portal_CPPFLAGS = \
	-DGETTEXT_PACKAGE=\"$(GETTEXT_PACKAGE)\"        \
	-DLOCALEDIR=\"$(localedir)\"			\
	$(NULL)

xdg_desktop_portal_validate_icon_SOURCES = src/validate-icon.c
xdg_desktop_portal_validate_icon_LDADD = $(GDK_PIXBUF_LIBS)
xdg_desktop_portal_validate_icon_CFLAGS = $(GDK_PIXBUF_CFLAGS)
