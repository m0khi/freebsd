/*-
 * Copyright (c) 2012 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 * iSCSI Common Layer.  It's used by both the initiator and target to send
 * and receive iSCSI PDUs.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/sx.h>

#include <dev/iscsi/icl.h>
#include <icl_conn_if.h>

struct icl_module {
	TAILQ_ENTRY(icl_module)		im_next;
	char				*im_name;
	int				im_priority;
	int				(*im_limits)(size_t *limitp);
	struct icl_conn			*(*im_new_conn)(const char *name,
					    struct mtx *lock);
};

struct icl_softc {
	struct sx			sc_lock;
	TAILQ_HEAD(, icl_module)	sc_modules;
};

static int sysctl_kern_icl_drivers(SYSCTL_HANDLER_ARGS);
static MALLOC_DEFINE(M_ICL, "icl", "iSCSI Common Layer");
static struct icl_softc	*sc;

SYSCTL_NODE(_kern, OID_AUTO, icl, CTLFLAG_RD, 0, "iSCSI Common Layer");
int icl_debug = 1;
SYSCTL_INT(_kern_icl, OID_AUTO, debug, CTLFLAG_RWTUN,
    &icl_debug, 0, "Enable debug messages");
SYSCTL_PROC(_kern_icl, OID_AUTO, drivers,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE,
    NULL, 0, sysctl_kern_icl_drivers, "A",
    "List of ICL drivers");

static int
sysctl_kern_icl_drivers(SYSCTL_HANDLER_ARGS)
{
	const struct icl_module *im;
	struct sbuf sb;
	int error;

	sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND | SBUF_INCLUDENUL);

	sx_slock(&sc->sc_lock);
	TAILQ_FOREACH(im, &sc->sc_modules, im_next) {
		if (im != TAILQ_FIRST(&sc->sc_modules))
			sbuf_putc(&sb, ' ');
		sbuf_printf(&sb, "%s", im->im_name);
	}
	sx_sunlock(&sc->sc_lock);

	error = sbuf_finish(&sb);
	if (error == 0)
		error = SYSCTL_OUT(req, sbuf_data(&sb), sbuf_len(&sb));
	sbuf_delete(&sb);
	return (error);
}

static struct icl_module *
icl_find(const char *name)
{
	struct icl_module *im, *im_max;

	sx_assert(&sc->sc_lock, SA_LOCKED);

	/*
	 * If the name was not specified, pick a module with highest
	 * priority.
	 */
	if (name == NULL || name[0] == '\0') {
		im_max = TAILQ_FIRST(&sc->sc_modules);
		TAILQ_FOREACH(im, &sc->sc_modules, im_next) {
			if (im->im_priority > im_max->im_priority)
				im_max = im;
		}

		return (im_max);
	}

	TAILQ_FOREACH(im, &sc->sc_modules, im_next) {
		if (strcasecmp(im->im_name, name) == 0)
			return (im);
	}

	return (NULL);
}

struct icl_conn *
icl_new_conn(const char *offload, const char *name, struct mtx *lock)
{
	struct icl_module *im;
	struct icl_conn *ic;

	sx_slock(&sc->sc_lock);
	im = icl_find(offload);

	if (im == NULL) {
		ICL_WARN("offload \"%s\" not found", offload);
		sx_sunlock(&sc->sc_lock);
		return (NULL);
	}

	ic = im->im_new_conn(name, lock);
	sx_sunlock(&sc->sc_lock);

	return (ic);
}

int
icl_limits(const char *offload, size_t *limitp)
{
	struct icl_module *im;
	int error;

	sx_slock(&sc->sc_lock);
	im = icl_find(offload);

	if (im == NULL) {
		ICL_WARN("offload \"%s\" not found", offload);
		sx_sunlock(&sc->sc_lock);
		return (ENXIO);
	}

	error = im->im_limits(limitp);
	sx_sunlock(&sc->sc_lock);

	return (error);
}


int
icl_register(const char *offload, int priority, int (*limits)(size_t *),
    struct icl_conn *(*new_conn)(const char *, struct mtx *))
{
	struct icl_module *im;

	sx_xlock(&sc->sc_lock);
	im = icl_find(offload);

	if (im != NULL) {
		ICL_WARN("offload \"%s\" already registered", offload);
		sx_xunlock(&sc->sc_lock);
		return (EBUSY);
	}

	im = malloc(sizeof(*im), M_ICL, M_ZERO | M_WAITOK);
	im->im_name = strdup(offload, M_ICL);
	im->im_priority = priority;
	im->im_limits = limits;
	im->im_new_conn = new_conn;

	TAILQ_INSERT_HEAD(&sc->sc_modules, im, im_next);
	sx_xunlock(&sc->sc_lock);

	ICL_DEBUG("offload \"%s\" registered", offload);
	return (0);
}

int
icl_unregister(const char *offload)
{
	struct icl_module *im;

	sx_xlock(&sc->sc_lock);
	im = icl_find(offload);

	if (im == NULL) {
		ICL_WARN("offload \"%s\" not registered", offload);
		sx_xunlock(&sc->sc_lock);
		return (ENXIO);
	}

	TAILQ_REMOVE(&sc->sc_modules, im, im_next);
	sx_xunlock(&sc->sc_lock);

	free(im->im_name, M_ICL);
	free(im, M_ICL);

	ICL_DEBUG("offload \"%s\" unregistered", offload);
	return (0);
}

static int
icl_load(void)
{

	sc = malloc(sizeof(*sc), M_ICL, M_ZERO | M_WAITOK);
	sx_init(&sc->sc_lock, "icl");
	TAILQ_INIT(&sc->sc_modules);

	return (0);
}

static int
icl_unload(void)
{

	sx_slock(&sc->sc_lock);
	KASSERT(TAILQ_EMPTY(&sc->sc_modules), ("still have modules"));
	sx_sunlock(&sc->sc_lock);

	sx_destroy(&sc->sc_lock);
	free(sc, M_ICL);

	return (0);
}

static int
icl_modevent(module_t mod, int what, void *arg)
{

	switch (what) {
	case MOD_LOAD:
		return (icl_load());
	case MOD_UNLOAD:
		return (icl_unload());
	default:
		return (EINVAL);
	}
}

moduledata_t icl_data = {
	"icl",
	icl_modevent,
	0
};

DECLARE_MODULE(icl, icl_data, SI_SUB_DRIVERS, SI_ORDER_FIRST);
MODULE_VERSION(icl, 1);
