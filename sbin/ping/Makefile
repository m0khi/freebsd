#	@(#)Makefile	8.1 (Berkeley) 6/5/93
# $FreeBSD$

.include <src.opts.mk>

PROG=	ping
MAN=	ping.8
BINOWN=	root
BINMODE=4555
WARNS?=	3
LIBADD=	m

.if ${MK_CASPER} != "no" && !defined(RESCUE)
LIBADD+=	casper
LIBADD+=	cap_dns
CFLAGS+=-DHAVE_LIBCASPER
.endif

.if !defined(RELEASE_CRUNCH)
CFLAGS+=-DIPSEC
LIBADD+=	ipsec
.endif

.include <bsd.prog.mk>
