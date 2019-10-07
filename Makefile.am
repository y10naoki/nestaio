noinst_PROGRAMS = nestaio

nestaio_SOURCES = src/main.c \
                  src/memcached.c \
                  src/nio_command.c \
                  src/nio_config.c \
                  src/nio_server.c \
                  src/nio_server.h

nestaio_CFLAGS = -I. -I@NESTALIB_HEADERS@

EXTRA_DIR = conf logs data

DISTCLEANFILES = *~

dist-hook:
	for subdir in $(EXTRA_DIR); do \
		cp -rp $$subdir $(distdir); \
		rm -f $(distdir)/$$subdir/*.txt; \
		rm -f $(distdir)/$$subdir/*.hdb; \
		rm -f $(distdir)/$$subdir/*~; \
		rm -f $(distdir)/$$subdir/.DS*; \
	done
