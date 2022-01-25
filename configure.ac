AC_PREREQ([2.63])

AC_INIT([xdg-desktop-portal],[1.13.0])

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

AC_ARG_WITH(flatpak_interfaces_dir,
        AS_HELP_STRING([--with-flatpak-interfaces-dir=PATH],[choose directory for Flatpak interface files, [default=PREFIX/share/dbus-1/interfaces]]),
        [[FLATPAK_INTERFACES_DIR="$withval"]],
	[PKG_CHECK_VAR([FLATPAK_INTERFACES_DIR], [flatpak >= 1.5.0], [interfaces_dir])])
AC_SUBST(FLATPAK_INTERFACES_DIR)

AC_ARG_WITH([systemduserunitdir],
            [AS_HELP_STRING([--with-systemduserunitdir=DIR],
                            [Directory for systemd user service files (default=PREFIX/lib/systemd/user)])],
    [],
    dnl This is deliberately not ${libdir}: systemd units always go in
    dnl .../lib, never .../lib64 or .../lib/x86_64-linux-gnu
    [with_systemduserunitdir='${prefix}/lib/systemd/user'])
AC_SUBST([systemduserunitdir], [$with_systemduserunitdir])

AC_PATH_PROG([BWRAP], [bwrap])

AC_SUBST([GLIB_COMPILE_RESOURCES], [`$PKG_CONFIG --variable glib_compile_resources gio-2.0`])
AC_SUBST([GDBUS_CODEGEN], [`$PKG_CONFIG --variable gdbus_codegen gio-2.0`])

PKG_CHECK_MODULES(BASE, [glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0])
AC_SUBST(BASE_CFLAGS)
AC_SUBST(BASE_LIBS)

PKG_CHECK_MODULES(GLIB260, glib-2.0 >= 2.60,
                  [AC_DEFINE(GLIB_VERSION_MIN_REQUIRED, GLIB_VERSION_2_60, [Ignore massive GTimeVal deprecation warnings in 2.62])],
                  [true])

AC_ARG_ENABLE(libportal,
	      [AS_HELP_STRING([--enable-libportal],[Enable libportal support. Needed for tests])],
	      enable_libportal=$enableval, enable_libportal=yes)
if test x$enable_libportal = xyes ; then
	PKG_CHECK_MODULES(LIBPORTAL, [libportal])
	AC_DEFINE([HAVE_LIBPORTAL],[1], [Define to enable libportal support])
fi
AM_CONDITIONAL([HAVE_LIBPORTAL],[test "$enable_libportal" = "yes"])

AC_ARG_ENABLE(geoclue,
	      [AS_HELP_STRING([--enable-geoclue],[Enable Geoclue support. Needed for location portal])],
	      enable_geoclue=$enableval, enable_geoclue=yes)
if test x$enable_geoclue = xyes ; then
	PKG_CHECK_MODULES(GEOCLUE, [libgeoclue-2.0 >= 2.5.2])
	AC_DEFINE([HAVE_GEOCLUE],[1], [Define to enable Geoclue support])
fi
AM_CONDITIONAL([HAVE_GEOCLUE],[test "$enable_geoclue" = "yes"])

AC_ARG_ENABLE(pipewire,
	      [AS_HELP_STRING([--enable-pipewire],[Enable PipeWire support. Needed for screen cast portal])],
	      enable_pipewire=$enableval, enable_pipewire=yes)
if test x$enable_pipewire = xyes ; then
	PKG_CHECK_MODULES(PIPEWIRE, [libpipewire-0.3 >= 0.2.90])
	AC_DEFINE([HAVE_PIPEWIRE],[1], [Define to enable PipeWire support])
fi
AM_CONDITIONAL([HAVE_PIPEWIRE],[test "$enable_pipewire" = "yes"])

AC_ARG_ENABLE(docbook-docs,
        [AS_HELP_STRING([--enable-docbook-docs],[build documentation (requires xmlto)])],
        enable_docbook_docs=$enableval, enable_docbook_docs=auto)
AC_PATH_PROG(XMLTO, xmlto, no)
AC_MSG_CHECKING([whether to build DocBook documentation])
if test x$XMLTO = xno ; then
        have_xmlto=no
else
        have_xmlto=yes
fi
if test x$enable_docbook_docs = xauto ; then
        if test x$have_xmlto = xno ; then
                enable_docbook_docs=no
        else
                if test ! -f "$srcdir/org.freedesktop.portal.Flatpak.xml" ; then
                        if test -z "$FLATPAK_INTERFACES_DIR" -o ! -f "$FLATPAK_INTERFACES_DIR/org.freedesktop.portal.Flatpak.xml" ; then
                                enable_docbook_docs=no
                        else
                                enable_docbook_docs=yes
                        fi
                else
                        enable_docbook_docs=yes
                fi
        fi
fi
if test x$enable_docbook_docs = xyes; then
        if test x$have_xmlto = xno; then
                AC_MSG_ERROR([Building DocBook docs explicitly required, but xmlto not found])
        fi
        if test ! -f "$srcdir/org.freedesktop.portal.Flatpak.xml" ; then
                if test -z "$FLATPAK_INTERFACES_DIR"; then
                        AC_MSG_ERROR([Flatpak development files are required to build DocBook docs])
                fi
                if test ! -f "$FLATPAK_INTERFACES_DIR/org.freedesktop.portal.Flatpak.xml" ; then
                        AC_MSG_ERROR([Flatpak development files are not correctly installed at $FLATPAK_INTERFACES_DIR])
                fi
        fi

        AC_MSG_RESULT(yes)
else
        AC_MSG_RESULT(no)
fi
AM_CONDITIONAL(DOCBOOK_DOCS_ENABLED, test x$enable_docbook_docs = xyes)
AC_ARG_VAR([XMLTO],[Define/override the 'xmlto' location.])
AC_ARG_VAR([XMLTO_FLAGS],[Define/override 'xmlto' options, like '--skip-validation'.])

##########################################
# Coverage testing
##########################################
AC_ARG_ENABLE(coverage,
  AS_HELP_STRING([--enable-coverage],
                 [enable coverage testing with gcov]),
  [use_lcov=$enableval], [use_lcov=no])

if test x$use_lcov = xyes; then
  AC_PATH_PROG(LCOV, lcov)
  AC_PATH_PROG(GENHTML, genhtml)

  # remove all optimization from CFLAGS
  changequote({,})
  CFLAGS=`echo "$CFLAGS" | $SED -e 's/-O[0-9]*//g'`
  changequote([,])

  CFLAGS="$CFLAGS -O0 -fprofile-arcs -ftest-coverage"
  LDFLAGS="$LDFLAGS -lgcov"
fi

GLIB_TESTS

PKG_CHECK_MODULES(FUSE3, [fuse3 >= 3.10.0])
AC_CHECK_FUNCS(renameat2)

AC_CONFIG_FILES([
Makefile
xdg-desktop-portal.pc
doc/Makefile
doc/portal-docs.xml
po/Makefile.in
])
AC_OUTPUT
