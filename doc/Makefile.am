NULL =

portal_files = 								\
	$(top_srcdir)/data/org.freedesktop.portal.Request.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.FileChooser.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.OpenURI.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.Print.xml 		\
	$(top_srcdir)/data/org.freedesktop.portal.Screenshot.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.NetworkMonitor.xml 	\
	$(top_srcdir)/data/org.freedesktop.portal.ProxyResolver.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Request.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.FileChooser.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.AppChooser.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Print.xml 	\
	$(top_srcdir)/data/org.freedesktop.impl.portal.Screenshot.xml 	\
	$(NULL)

xml_files = 								\
	portal-org.freedesktop.portal.Request.xml 			\
	portal-org.freedesktop.portal.FileChooser.xml 			\
	portal-org.freedesktop.portal.OpenURI.xml 			\
	portal-org.freedesktop.portal.Print.xml 			\
	portal-org.freedesktop.portal.Screenshot.xml 			\
	portal-org.freedesktop.portal.NetworkMonitor.xml		\
	portal-org.freedesktop.portal.ProxyResolver.xml			\
	portal-org.freedesktop.impl.portal.Request.xml 			\
	portal-org.freedesktop.impl.portal.FileChooser.xml		\
	portal-org.freedesktop.impl.portal.AppChooser.xml 		\
	portal-org.freedesktop.impl.portal.Print.xml 			\
	portal-org.freedesktop.impl.portal.Screenshot.xml 		\
	$(NULL)

EXTRA_DIST = \
	version.xml.in		\
	docbook.css		\
	xmlto-config.xsl	\
	$(NULL)

$(xml_files) : $(portal_files)
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

portal-docs.html: portal-docs.xml version.xml $(xml_files) xmlto-config.xsl
	$(AM_V_GEN) $(XMLTO) xhtml-nochunks -m xmlto-config.xsl portal-docs.xml

endif # DOCBOOK_DOCS_ENABLED
