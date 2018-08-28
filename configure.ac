AC_PREREQ([2.63])

AC_INIT([xdg-desktop-portal],[1.0.1])

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

AC_ARG_ENABLE(pipewire,
	      [AS_HELP_STRING([--enable-pipewire],[Enable PipeWire support. Needed for screen cast portal])],
	      enable_pipewire=$enableval, enable_pipewire=yes)
if test x$enable_pipewire = xyes ; then
	PKG_CHECK_MODULES(PIPEWIRE, [libpipewire-0.2 >= 0.2.2])
	AC_DEFINE([HAVE_PIPEWIRE],[1], [Define to enable PipeWire support])
fi
AM_CONDITIONAL([HAVE_PIPEWIRE],[test "$enable_pipewire" = "yes"])

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

# Enable lcov coverage reports
AC_ARG_ENABLE(coverage,
  AS_HELP_STRING([--enable-coverage],
		 [enable covergae testing with gcov]),
  [use_gcov=$enableval], [use_gcov=no])

AS_IF([ test "x$use_gcov" = "xyes"], [
  dnl we need gcc:
  if test "$GCC" != "yes"; then
    AC_MSG_ERROR([GCC is required for --enable-coverage])
  fi

  dnl Check if ccache is being used
  AC_CHECK_PROG(SHTOOL, shtool, shtool)
  case `$SHTOOL path $CC` in
    *ccache*[)] gcc_ccache=yes;;
    *[)] gcc_ccache=no;;
  esac

  if test "$gcc_ccache" = "yes" && (test -z "$CCACHE_DISABLE" || test "$CCACHE_DISABLE" != "1"); then
    AC_MSG_ERROR([ccache must be disabled when --enable-coverage option is used. You can disable ccache by setting environment variable CCACHE_DISABLE=1.])
  fi

  ltp_version_list="1.6 1.7 1.8 1.9 1.10 1.12"
  AC_CHECK_PROG(LTP, lcov, lcov)
  AC_CHECK_PROG(LTP_GENHTML, genhtml, genhtml)

  AS_IF([ test "$LTP" ], [
    AC_CACHE_CHECK([for ltp version], glib_cv_ltp_version, [
      glib_cv_ltp_version=invalid
      ltp_version=`$LTP -v 2>/dev/null | $SED -e 's/^.* //'`
      for ltp_check_version in $ltp_version_list; do
        if test "$ltp_version" = "$ltp_check_version"; then
          glib_cv_ltp_version="$ltp_check_version (ok)"
        fi
      done
    ])
  ], [
    ltp_msg="To enable code coverage reporting you must have one of the following LTP versions installed: $ltp_version_list"
    AC_MSG_ERROR([$ltp_msg])
  ])

  case $glib_cv_ltp_version in
    ""|invalid[)]
      ltp_msg="You must have one of the following versions of LTP: $ltp_version_list (found: $ltp_version)."
      AC_MSG_ERROR([$ltp_msg])
      LTP="exit 0;"
      ;;
  esac

  if test -z "$LTP_GENHTML"; then
    AC_MSG_ERROR([Could not find genhtml from the LTP package])
  fi

  dnl Remove all optimization flags from CFLAGS
  changequote({,})
  CFLAGS=`echo "$CFLAGS" | $SED -e 's/-O[0-9]*//g'`
  changequote([,])

  dnl Add the special gcc flags
  CFLAGS="$CFLAGS -O0 -fprofile-arcs -ftest-coverage"
  LDFLAGS="$LDFLAGS -lgcov"
])

GLIB_TESTS

PKG_CHECK_MODULES(FUSE, [fuse])

AC_CONFIG_FILES([
Makefile
xdg-desktop-portal.pc
doc/Makefile
doc/portal-docs.xml
po/Makefile.in
])
AC_OUTPUT
