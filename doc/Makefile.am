NULL =

include $(top_srcdir)/Makefile.am.inc

org.freedesktop.portal.Flatpak.xml :
	cp $(FLATPAK_INTERFACES_DIR)/org.freedesktop.portal.Flatpak.xml .

portal_files = 								\
	org.freedesktop.portal.Flatpak.xml		 		\
	$(PORTAL_IFACE_FILES) \
	$(PORTAL_IMPL_IFACE_FILES) \
	$(BACKGROUND_MONITOR_IFACE_FILES) \
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
