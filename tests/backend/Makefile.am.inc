tests_test_backends_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS) -I$(top_builddir)/src
tests_test_backends_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(NULL)
tests_test_backends_SOURCES = \
	tests/backend/test-backends.c \
	tests/backend/request.h \
	tests/backend/request.c \
	tests/backend/session.h \
	tests/backend/session.c \
	tests/backend/access.c \
	tests/backend/access.h \
	tests/backend/account.c \
	tests/backend/account.h \
	tests/backend/appchooser.c \
	tests/backend/appchooser.h \
	tests/backend/background.c \
	tests/backend/background.h \
	tests/backend/email.c \
	tests/backend/email.h \
	tests/backend/filechooser.c \
	tests/backend/filechooser.h \
	tests/backend/inhibit.c \
	tests/backend/inhibit.h \
	tests/backend/lockdown.c \
	tests/backend/lockdown.h \
	tests/backend/notification.c \
	tests/backend/notification.h \
	tests/backend/print.c \
	tests/backend/print.h \
	tests/backend/screenshot.c \
	tests/backend/screenshot.h \
	tests/backend/settings.c \
	tests/backend/settings.h \
	tests/backend/wallpaper.c \
	tests/backend/wallpaper.h \
	tests/glib-backports.c \
	tests/glib-backports.h \
	$(NULL)
nodist_tests_test_backends_SOURCES = src/xdp-impl-dbus.c

# We build this in a tests/ subdirectory so that it can be accessed
# via G_TEST_BUILT
test_extra_programs += \
	tests/test-backends \
	$(NULL)
