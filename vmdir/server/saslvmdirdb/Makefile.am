vmdirsasl_LTLIBRARIES = libsaslvmdirdb.la

libsaslvmdirdb_la_CPPFLAGS = \
    -I$(top_srcdir)/vmdir/include \
    -I$(top_srcdir)/vmdir/include/public \
    @OPENSSL_INCLUDES@ \
    @LW_INCLUDES@

libsaslvmdirdb_la_SOURCES = \
    vmdirdb_init.c \
    vmdirdb.c

libsaslvmdirdb_la_LIBADD = \
    $(top_builddir)/vmdir/client/libvmdirclient.la \
    @CRYPTO_LIBS@ \
    @PTHREAD_LIBS@

libsaslvmdirdb_la_LDFLAGS = \
    @DCERPC_LDFLAGS@ \
    @OPENSSL_LDFLAGS@ \
    @LW_LDFLAGS@
