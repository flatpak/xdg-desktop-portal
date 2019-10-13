test_backends_CFLAGS = $(AM_CFLAGS) $(BASE_CFLAGS)
test_backends_LDADD = \
	$(AM_LDADD) \
	$(BASE_LIBS) \
	$(NULL)
test_backends_SOURCES = \
	tests/backend/test-backends.c \
	tests/backend/account.c \
	tests/backend/account.h \
	tests/backend/request.h \
	tests/backend/request.c \
	$(NULL)
nodist_test_backends_SOURCES = src/xdp-impl-dbus.c

noinst_PROGRAMS += \
	test-backends \
	$(NULL)
