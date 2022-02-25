NULL =

org.freedesktop.portal.Flatpak.xml :
	cp $(FLATPAK_INTERFACES_DIR)/org.freedesktop.portal.Flatpak.xml .

portal_files = 								\
	org.freedesktop.portal.Flatpak.xml		 		\
	$(top_srcdir)/data/org.freedesktop.portal.Account.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.Background.xml		\
	$(top_srcdir)/data/org.freedesktop.portal.Camera.xml		\
	$(top_srcdir)/data/org.freedesktop.portal.Device.xml	 	\
	$(top_srcdir)/data/org.freedesktop.portal.Documents.xml		\
	$(top_srcdir)/data/org.freedesktop.portal.Email.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.FileChooser.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.FileTransfer.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.GameMode.xml		\
	$(top_srcdir)/data/org.freedesktop.portal.Inhibit.xml	 	\
	$(top_srcdir)/data/org.freedesktop.portal.Location.xml		\
	$(top_srcdir)/data/org.freedesktop.portal.MemoryMonitor.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.NetworkMonitor.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.Notification.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.OpenURI.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.PowerProfileMonitor.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.Print.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.ProxyResolver.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.Realtime.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.RemoteDesktop.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.Request.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.ScreenCast.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.Screenshot.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.Secret.xml		\
	$(top_srcdir)/data/org.freedesktop.portal.Session.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.Settings.xml		\
	$(top_srcdir)/data/org.freedesktop.portal.Trash.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.Wallpaper.xml		\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Access.xml	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Account.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.AppChooser.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Background.xml		\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Email.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.FileChooser.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Inhibit.xml	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Lockdown.xml	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Notification.xml	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.PermissionStore.xml	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Print.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.RemoteDesktop.xml\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Request.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.ScreenCast.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Screenshot.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Secret.xml	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Session.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Settings.xml	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Wallpaper.xml		\
	$(NULL)

# Sub-interface of org.freedesktop.portal.Flatpak.xml
extra_xml_files = org.freedesktop.portal.Flatpak.UpdateMonitor.xml
xml_files = $(addprefix portal-, $(notdir $(portal_files) $(extra_xml_files)))

EXTRA_DIST = \
	org.freedesktop.portal.Flatpak.xml \
	docbook.css		\
	portal-docs.xml.in	\
	xmlto-config.xsl	\
	$(NULL)

$(xml_files) : $(portal_files)
	$(AM_V_GEN) $(GDBUS_CODEGEN) --generate-docbook portal $^

CLEANFILES =			\
	$(xml_files) 		\
	portal-docs.html	\
	$(NULL)

DISTCLEANFILES =	\
	portal-docs.xml	\
	$(NULL)

if DOCBOOK_DOCS_ENABLED

doc_DATA = 			\
	portal-docs.html	\
	docbook.css		\
	$(NULL)

portal-docs.html: portal-docs.xml $(xml_files) xmlto-config.xsl
	$(AM_V_GEN) $(XMLTO) $(XMLTO_FLAGS) xhtml-nochunks -m $(srcdir)/xmlto-config.xsl $<

endif # DOCBOOK_DOCS_ENABLED
