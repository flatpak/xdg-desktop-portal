AC_PREREQ([2.63])

AC_INIT([xdg-desktop-portal],[0.9])

AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
AM_PROG_CC_C_O
AC_DISABLE_STATIC

LT_PREREQ([2.2.6])
LT_INIT([disable-static])

AC_CONFIG_SRCDIR([src/xdg-desktop-portal.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([1.11 no-define no-dist-gzip dist-xz tar-ustar foreign subdir-objects])
AC_PROG_SED


# Enable silent rules is available
AM_SILENT_RULES([yes])
AM_MAINTAINER_MODE([enable])

if test "x$GCC" = "xyes"; then
  case " $CFLAGS " in
  *[[\ \	]]-Wall[[\ \	]]*) ;;
  *) CFLAGS="$CFLAGS -Wall" ;;
  esac
fi

# i18n stuff
GETTEXT_PACKAGE=xdg-desktop-portal
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, "$GETTEXT_PACKAGE",
                   [The prefix for our gettext translation domains.])

AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION(0.18.3)

PKG_PROG_PKG_CONFIG([0.24])

AC_ARG_WITH(dbus_service_dir,
        AS_HELP_STRING([--with-dbus-service-dir=PATH],[choose directory for dbus service files, [default=PREFIX/share/dbus-1/services]]),
        with_dbus_service_dir="$withval", with_dbus_service_dir=$datadir/dbus-1/services)
DBUS_SERVICE_DIR=$with_dbus_service_dir
AC_SUBST(DBUS_SERVICE_DIR)

AC_ARG_WITH([systemduserunitdir],
            [AS_HELP_STRING([--with-systemduserunitdir=DIR],
                            [Directory for systemd user service files (default=PREFIX/lib/systemd/user)])],
    [],
    dnl This is deliberately not ${libdir}: systemd units always go in
    dnl .../lib, never .../lib64 or .../lib/x86_64-linux-gnu
    [with_systemduserunitdir='${prefix}/lib/systemd/user'])
AC_SUBST([systemduserunitdir], [$with_systemduserunitdir])

AC_SUBST([GLIB_COMPILE_RESOURCES], [`$PKG_CONFIG --variable glib_compile_resources gio-2.0`])
AC_SUBST([GDBUS_CODEGEN], [`$PKG_CONFIG --variable gdbus_codegen gio-2.0`])

PKG_CHECK_MODULES(BASE, [glib-2.0 gio-2.0 gio-unix-2.0])
AC_SUBST(BASE_CFLAGS)
AC_SUBST(BASE_LIBS)

PKG_CHECK_EXISTS([flatpak],
                 [AC_SUBST([FLATPAK_INTERFACES_DIR],
                           [`$PKG_CONFIG --variable=interfaces_dir flatpak`])],
                 [AC_MSG_ERROR([flatpak.pc is required])])

AC_ARG_ENABLE(docbook-docs,
        [AS_HELP_STRING([--enable-docbook-docs],[build documentation (requires xmlto)])],
        enable_docbook_docs=$enableval, enable_docbook_docs=auto)
AC_PATH_PROG(XMLTO, xmlto, no)
AC_MSG_CHECKING([whether to build DocBook documentation])
if test x$XMLTO = xno ; then
        have_docbook=no
else
        have_docbook=yes
fi
if test x$enable_docbook_docs = xauto ; then
        if test x$have_docbook = xno ; then
                enable_docbook_docs=no
        else
                enable_docbook_docs=yes
        fi
fi
if test x$enable_docbook_docs = xyes; then
        if test x$have_docbook = xno; then
                AC_MSG_ERROR([Building DocBook docs explicitly required, but DocBook not found])
        fi
        AC_MSG_RESULT(yes)
else
        AC_MSG_RESULT(no)
fi
AM_CONDITIONAL(DOCBOOK_DOCS_ENABLED, test x$enable_docbook_docs = xyes)
AC_ARG_VAR([XMLTO],[Define/override the 'xmlto' location.])
AC_ARG_VAR([XMLTO_FLAGS],[Define/override 'xmlto' options, like '--skip-validation'.])

AC_CONFIG_FILES([
Makefile
xdg-desktop-portal.pc
doc/Makefile
doc/portal-docs.xml
po/Makefile.in
])
AC_OUTPUT
