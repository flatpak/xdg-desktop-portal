NULL =

PORTAL_INTERFACES = 							\
	$(top_srcdir)/data/org.freedesktop.portal.Request.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.FileChooser.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.OpenURI.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.Print.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.Screenshot.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.NetworkMonitor.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.ProxyResolver.xml 	\
	$(NULL)

xml_files = 								\
	portal-org.freedesktop.portal.Request.xml 			\
	portal-org.freedesktop.portal.FileChooser.xml 			\
	portal-org.freedesktop.portal.OpenURI.xml 			\
	portal-org.freedesktop.portal.Print.xml 			\
	portal-org.freedesktop.portal.Screenshot.xml 			\
	portal-org.freedesktop.portal.NetworkMonitor.xml		\
	portal-org.freedesktop.portal.ProxyResolver.xml			\
	$(NULL)

EXTRA_DIST = \
	version.xml.in		\
	docbook.css		\
	xmlto-config.xsl	\
	$(NULL)

$(xml_files) : $(PORTAL_INTERFACES)
	$(AM_V_GEN) $(GDBUS_CODEGEN) --generate-docbook portal $^

CLEANFILES =			\
	$(xml_files) 		\
	portal-docs.html	\
	$(NULL)

DISTCLEANFILES =	\
	version.xml	\
	$(NULL)

if DOCBOOK_DOCS_ENABLED

doc_DATA = 			\
	portal-docs.html	\
	docbook.css		\
	$(NULL)

portal-docs.html: portal-docs.xml version.xml $(xml_files)
	$(AM_V_GEN) $(XMLTO) xhtml-nochunks -m xmlto-config.xsl portal-docs.xml

endif # DOCBOOK_DOCS_ENABLED
