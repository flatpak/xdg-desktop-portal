# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = "XDG Desktop Portal"
copyright = "2023, XDG Desktop Portal authors"
author = "XDG Desktop Portal authors"

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration
html_favicon = "favicon.ico"

extensions = [
    "sphinxext.opengraph",
    "sphinx_copybutton",
]

templates_path = ["_templates"]
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]


# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = "furo"
# add custom files that are stored in _static
html_css_files = ["xdg.css"]
html_static_path = ["_static"]
html_favicon = "img/favicon.svg"
html_logo = "img/logo.svg"

# -- Options for OpenGraph ---------------------------------------------------

ogp_site_url = "https://flatpak.github.io/xdg-desktop-portal/docs/"
ogp_image = "_static/card.png"
