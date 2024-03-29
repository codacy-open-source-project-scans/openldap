# $OpenLDAP$
# This work is part of OpenLDAP Software <http://www.openldap.org/>.
#
# Copyright 2003-2024 The OpenLDAP Foundation.
# Portions Copyright 2004 by IBM Corporation.
# All rights reserved.

# Copyright 2004 Sang Seok Lim, IBM Corp. All Rights Reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted only as authorized by the OpenLDAP
# Public License.
#
# A copy of this license is available in the file LICENSE in the
# top-level directory of the distribution or, alternatively, at
# <http://www.OpenLDAP.org/license.html>.

LDAP_SRC = ../../..
LDAP_BUILD = $(LDAP_SRC)
LDAP_INC = -I$(LDAP_BUILD)/include -I$(LDAP_SRC)/include -I$(LDAP_SRC)/servers/slapd
LDAP_LIB = $(LDAP_BUILD)/libraries/libldap/libldap.la \
	$(LDAP_BUILD)/libraries/liblber/liblber.la

SNACC_DIR = ../$(LDAP_SRC)/snacc
SNACC_INC = -I$(SNACC_DIR) -I$(SNACC_DIR)/c-lib/inc
SNACC_LIB = $(SNACC_DIR)/c-lib/libcasn1.a

SSL_DIR = /usr/local
SSL_INC = -I$(SSL_DIR)/include/openssl
SSL_LIB = -lcrypto -L$(SSL_DIR)/lib

PLAT = UNIX
NT_LIB = -L$(LDAP_BUILD)/servers/slapd -lslapd
NT_LDFLAGS = -no-undefined -avoid-version
UNIX_LDFLAGS = -version-info $(LTVER)

LIBTOOL = $(LDAP_BUILD)/libtool
CC = gcc
OPT = -g -O2
DEFS = -DLDAP_COMPONENT
INCS = $(LDAP_INC) $(SNACC_INC) $(SSL_INC)
LIBS = $($(PLAT)_LIB) $(LDAP_LIB) $(SNACC_LIB) $(SSL_LIB)
LD_FLAGS = $(LDFLAGS) $($(PLAT)_LDFLAGS) -rpath $(moduledir) -module

PROGRAMS = compmatch.la
LTVER = 0:0:0

prefix=/usr/local
exec_prefix=$(prefix)
ldap_subdir=/openldap

libdir=$(exec_prefix)/lib
libexecdir=$(exec_prefix)/libexec
moduledir = $(libexecdir)$(ldap_subdir)

.SUFFIXES: .c .o .lo

.c.lo:
	$(LIBTOOL) --mode=compile $(CC) $(CFLAGS) $(OPT) $(CPPFLAGS) $(DEFS) $(INCS) -c $<

all: $(PROGRAMS)

compmatch.la: componentlib.lo init.lo certificate.lo asn_to_syn_mr.lo authorityKeyIdentifier.lo crl.lo
	$(LIBTOOL) --mode=link $(CC) $(LD_FLAGS) -o $@ $? $(LIBS)

clean:
	rm -rf *.o *.lo *.la .libs

install: $(PROGRAMS)
	mkdir -p $(DESTDIR)$(moduledir)
	for p in $(PROGRAMS) ; do \
		$(LIBTOOL) --mode=install cp $$p $(DESTDIR)$(moduledir) ; \
	done

