/* $NetBSD: fdtbus.c,v 1.24 2018/09/23 19:32:03 jmcneill Exp $ */

/*-
 * Copyright (c) 2015 Jared D. McNeill <jmcneill@invisible.ca>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: fdtbus.c,v 1.24 2018/09/23 19:32:03 jmcneill Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>

#include <sys/bus.h>

#include <dev/ofw/openfirm.h>

#include <dev/fdt/fdtvar.h>

#include <libfdt.h>

#include "locators.h"

#define	FDT_MAX_PATH	256

struct fdt_node {
	device_t	n_bus;
	device_t	n_dev;
	int		n_phandle;
	const char	*n_name;
	struct fdt_attach_args n_faa;

	u_int		n_order;

	TAILQ_ENTRY(fdt_node) n_nodes;
};

static TAILQ_HEAD(, fdt_node) fdt_nodes =
    TAILQ_HEAD_INITIALIZER(fdt_nodes);
static bool fdt_need_rescan = false;

struct fdt_softc {
	device_t	sc_dev;
	int		sc_phandle;
	struct fdt_attach_args sc_faa;
};

static int	fdt_match(device_t, cfdata_t, void *);
static void	fdt_attach(device_t, device_t, void *);
static int	fdt_rescan(device_t, const char *, const int *);
static void	fdt_childdet(device_t, device_t);

static int	fdt_scan_submatch(device_t, cfdata_t, const int *, void *);
static void	fdt_scan(struct fdt_softc *, int);
static void	fdt_add_node(struct fdt_node *);
static u_int	fdt_get_order(int);

static const char * const fdtbus_compatible[] =
    { "simple-bus", "simple-mfd", NULL };

CFATTACH_DECL2_NEW(simplebus, sizeof(struct fdt_softc),
    fdt_match, fdt_attach, NULL, NULL, fdt_rescan, fdt_childdet);

static int
fdt_match(device_t parent, cfdata_t cf, void *aux)
{
	const struct fdt_attach_args *faa = aux;
	const int phandle = faa->faa_phandle;
	int match;

	/* Check compatible string */
	match = of_match_compatible(phandle, fdtbus_compatible);
	if (match)
		return match;

	/* Some nodes have no compatible string */
	if (!of_hasprop(phandle, "compatible")) {
		if (OF_finddevice("/clocks") == phandle)
			return 1;
		if (OF_finddevice("/chosen") == phandle)
			return 1;
	}

	/* Always match the root node */
	return OF_finddevice("/") == phandle;
}

static void
fdt_attach(device_t parent, device_t self, void *aux)
{
	struct fdt_softc *sc = device_private(self);
	const struct fdt_attach_args *faa = aux;
	const int phandle = faa->faa_phandle;
	const char *descr;

	sc->sc_dev = self;
	sc->sc_phandle = phandle;
	sc->sc_faa = *faa;

	aprint_naive("\n");

	descr = fdtbus_get_string(phandle, "model");
	if (descr)
		aprint_normal(": %s\n", descr);
	else
		aprint_normal("\n");

	/* Find all child nodes */
	fdt_add_bus(self, phandle, &sc->sc_faa);

	/* Only the root bus should scan for devices */
	if (OF_finddevice("/") != faa->faa_phandle)
		return;

	/* Scan devices */
	fdt_rescan(self, NULL, NULL);
}

static int
fdt_rescan(device_t self, const char *ifattr, const int *locs)
{
	struct fdt_softc *sc = device_private(self);
	int pass;

	pass = 0;
	fdt_need_rescan = false;
	do {
		fdt_scan(sc, pass);
		if (fdt_need_rescan == true) {
			pass = 0;
			fdt_need_rescan = false;
		} else {
			pass++;
		}
	} while (pass <= FDTCF_PASS_DEFAULT);

	return 0;
}

static void
fdt_childdet(device_t parent, device_t child)
{
	struct fdt_node *node;

	TAILQ_FOREACH(node, &fdt_nodes, n_nodes)
		if (node->n_dev == child) {
			node->n_dev = NULL;
			break;
		}
}

static void
fdt_init_attach_args(const struct fdt_attach_args *faa_tmpl, struct fdt_node *node,
    bool quiet, struct fdt_attach_args *faa)
{
	*faa = *faa_tmpl;
	faa->faa_phandle = node->n_phandle;
	faa->faa_name = node->n_name;
	faa->faa_quiet = quiet;
}

static bool
fdt_add_bus_stdmatch(void *arg, int child)
{
	return fdtbus_status_okay(child);
}

void
fdt_add_bus(device_t bus, const int phandle, struct fdt_attach_args *faa)
{
	fdt_add_bus_match(bus, phandle, faa, fdt_add_bus_stdmatch, NULL);
}

void
fdt_add_bus_match(device_t bus, const int phandle, struct fdt_attach_args *faa,
    bool (*fn)(void *, int), void *fnarg)
{
	struct fdt_node *node;
	int child;

	for (child = OF_child(phandle); child; child = OF_peer(child)) {
		if (fn && !fn(fnarg, child))
			continue;

		/* Add the node to our device list */
		node = kmem_alloc(sizeof(*node), KM_SLEEP);
		node->n_bus = bus;
		node->n_dev = NULL;
		node->n_phandle = child;
		node->n_name = fdtbus_get_string(child, "name");
		node->n_order = fdt_get_order(child);
		node->n_faa = *faa;
		node->n_faa.faa_phandle = child;
		node->n_faa.faa_name = node->n_name;

		fdt_add_node(node);
		fdt_need_rescan = true;
	}
}

static int
fdt_scan_submatch(device_t parent, cfdata_t cf, const int *locs, void *aux)
{
	if (locs[FDTCF_PASS] != FDTCF_PASS_DEFAULT &&
	    locs[FDTCF_PASS] != cf->cf_loc[FDTCF_PASS])
		return 0;

	return config_stdsubmatch(parent, cf, locs, aux);
}

static cfdata_t
fdt_scan_best(struct fdt_softc *sc, struct fdt_node *node)
{
	struct fdt_attach_args faa;
	cfdata_t cf, best_cf;
	int match, best_match;

	best_cf = NULL;
	best_match = 0;

	for (int pass = 0; pass <= FDTCF_PASS_DEFAULT; pass++) {
		const int locs[FDTCF_NLOCS] = {
			[FDTCF_PASS] = pass
		};
		fdt_init_attach_args(&sc->sc_faa, node, true, &faa);
		cf = config_search_loc(fdt_scan_submatch, node->n_bus, "fdt", locs, &faa);
		if (cf == NULL)
			continue;
		match = config_match(node->n_bus, cf, &faa);
		if (match > best_match) {
			best_match = match;
			best_cf = cf;
		}
	}

	return best_cf;
}

static void
fdt_scan(struct fdt_softc *sc, int pass)
{
	struct fdt_node *node;
	struct fdt_attach_args faa;
	const int locs[FDTCF_NLOCS] = {
		[FDTCF_PASS] = pass
	};
	bool quiet = pass != FDTCF_PASS_DEFAULT;
	prop_dictionary_t dict;
	char buf[FDT_MAX_PATH];

	TAILQ_FOREACH(node, &fdt_nodes, n_nodes) {
		if (node->n_dev != NULL)
			continue;

		fdt_init_attach_args(&sc->sc_faa, node, quiet, &faa);

		/*
		 * Make sure we don't attach before a better match in a later pass.
		 */
		cfdata_t cf_best = fdt_scan_best(sc, node);
		cfdata_t cf_pass =
		    config_search_loc(fdt_scan_submatch, node->n_bus, "fdt", locs, &faa);
		if (cf_best != cf_pass)
			continue;

		/*
		 * Attach the device.
		 */
		node->n_dev = config_found_sm_loc(node->n_bus, "fdt", locs,
		    &faa, fdtbus_print, fdt_scan_submatch);
		if (node->n_dev) {
			dict = device_properties(node->n_dev);
			if (fdtbus_get_path(node->n_phandle, buf, sizeof(buf)))
				prop_dictionary_set_cstring(dict, "fdt-path", buf);
		}
	}
}

static void
fdt_add_node(struct fdt_node *new_node)
{
	struct fdt_node *node;

	TAILQ_FOREACH(node, &fdt_nodes, n_nodes)
		if (node->n_order > new_node->n_order) {
			TAILQ_INSERT_BEFORE(node, new_node, n_nodes);
			return;
		}
	TAILQ_INSERT_TAIL(&fdt_nodes, new_node, n_nodes);
}

void
fdt_remove_byhandle(int phandle)
{
	struct fdt_node *node;

	TAILQ_FOREACH(node, &fdt_nodes, n_nodes) {
		if (node->n_phandle == phandle) {
			TAILQ_REMOVE(&fdt_nodes, node, n_nodes);
			return;
		}
	}
}

void
fdt_remove_bycompat(const char *compatible[])
{
	struct fdt_node *node, *next;

	TAILQ_FOREACH_SAFE(node, &fdt_nodes, n_nodes, next) {
		if (of_match_compatible(node->n_phandle, compatible)) {
			TAILQ_REMOVE(&fdt_nodes, node, n_nodes);
		}
	}
}

static u_int
fdt_get_order(int phandle)
{
	u_int val = UINT_MAX;
	int child;

	of_getprop_uint32(phandle, "phandle", &val);

	for (child = OF_child(phandle); child; child = OF_peer(child)) {
		u_int child_val = fdt_get_order(child);
		if (child_val < val)
			val = child_val;
	}

	return val;
}

int
fdtbus_print(void *aux, const char *pnp)
{
	const struct fdt_attach_args * const faa = aux;
	char buf[FDT_MAX_PATH];
	const char *name = buf;
	int len;

	if (pnp && faa->faa_quiet)
		return QUIET;

	/* Skip "not configured" for nodes w/o compatible property */
	if (pnp && OF_getproplen(faa->faa_phandle, "compatible") <= 0)
		return QUIET;

	if (!fdtbus_get_path(faa->faa_phandle, buf, sizeof(buf)))
		name = faa->faa_name;

	if (pnp) {
		aprint_normal("%s at %s", name, pnp);
		const char *compat = fdt_getprop(fdtbus_get_data(),
		    fdtbus_phandle2offset(faa->faa_phandle), "compatible",
		    &len);
		while (len > 0) {
			aprint_debug(" <%s>", compat);
			len -= (strlen(compat) + 1);
			compat += (strlen(compat) + 1);
		}
	} else
		aprint_debug(" (%s)", name);

	return UNCONF;
}
