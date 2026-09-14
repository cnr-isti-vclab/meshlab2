# Configuration file for the Sphinx documentation builder.
project = 'MeshLab'
copyright = '2026, MeshLab Developers'
author = 'MeshLab Developers'
release = '0.1'

extensions = ['myst_parser', 'sphinx.ext.mathjax']
myst_enable_extensions = ['dollarmath']

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store', 'generate_api.py']

html_theme = 'furo'
html_static_path = ['_static']
