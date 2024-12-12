Icons
========

Some portal APIs accept Icon image data either as bytes or memfd sealed file descriptors.
These icons must pass validation by ``xdg-desktop-portal-validate-icon`` in order to be used
successfully. The requirements to pass validation are:

.. csv-table:: Icon Requirements
   :header: "Icon Property", "Requirement", "Description"

   "Shape", "Square", "All icons, whether PNG, JPEG, or SVG, must be square."
   "Edge Length", "512 pixels", "For raster images, the maximum edge length is 512 pixels."
   "SVG File Size", "4096 bytes", "For SVG images, the data describing the SVG must fit within 4096 bytes."
   "Raster File Size", "4 MiB", "For raster images, the total data must fit within 4MB."
