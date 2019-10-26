test_backends_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
test_backends_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(NULL)
test_backends_SOURCES = \
	tests/backend/test-backends.c \
	tests/backend/request.h \
	tests/backend/request.c \
	tests/backend/access.c \
	tests/backend/access.h \
	tests/backend/account.c \
	tests/backend/account.h \
	tests/backend/email.c \
	tests/backend/email.h \
	tests/backend/filechooser.c \
	tests/backend/filechooser.h \
	tests/backend/inhibit.c \
	tests/backend/inhibit.h \
	tests/backend/lockdown.c \
	tests/backend/lockdown.h \
	tests/backend/print.c \
	tests/backend/print.h \
	tests/backend/screenshot.c \
	tests/backend/screenshot.h \
	$(NULL)
nodist_test_backends_SOURCES = src/xdp-impl-dbus.c

noinst_PROGRAMS += \
	test-backends \
	$(NULL)
