<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:fo="http://www.w3.org/1999/XSL/Format"
                exclude-result-prefixes="fo"
                version="1.0">
  <xsl:param name="html.stylesheet" select="'docbook.css'"/>

  <xsl:template name="system.head.content">
    <meta xmlns="http://www.w3.org/1999/xhtml"
          name="viewport"
          content="width=device-width, initial-scale=1.0"/>
  </xsl:template>
</xsl:stylesheet>
