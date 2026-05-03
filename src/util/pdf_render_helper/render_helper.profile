# render_helper.profile — minimal sandbox for xopp-render-helper
# Just the basics: no network, no GUI, drop caps. Nothing that could
# block poppler/glib startup.

net none
caps.drop all
nonewprivs
noroot
