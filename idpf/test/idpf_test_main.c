/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Test module entry point.
 *
 * Builds the driver objects a second time with -DIDPF_UNIT_TEST so that the
 * file-static functions in idpf_main.c are reachable. The inert module runs
 * suites only when sysctl hw.idpf_test.run is written. Results go to the
 * console and to hw.idpf_test.* so a script can read a verdict without
 * parsing dmesg.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include "idpf_test.h"

int idpf_test_checks;
int idpf_test_failures;
const char *idpf_test_current = "<none>";

static int idpf_test_cases_run;

static SYSCTL_NODE(_hw, OID_AUTO, idpf_test, CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
    "idpf unit tests");
SYSCTL_INT(_hw_idpf_test, OID_AUTO, checks, CTLFLAG_RD, &idpf_test_checks, 0,
    "assertions evaluated");
SYSCTL_INT(_hw_idpf_test, OID_AUTO, failures, CTLFLAG_RD, &idpf_test_failures,
    0, "assertions failed");
SYSCTL_INT(_hw_idpf_test, OID_AUTO, cases, CTLFLAG_RD, &idpf_test_cases_run, 0,
    "test cases run");

/**
 * idpf_test_run_suite - run every case in a suite
 * @suite: suite to run
 *
 * Return: number of assertions that failed in this suite.
 */
int
idpf_test_run_suite(const struct idpf_test_suite *suite)
{
	int before = idpf_test_failures;
	int i;

	printf("idpf_test: suite %s (%d cases)\n", suite->name, suite->ncases);

	for (i = 0; i < suite->ncases; i++) {
		int case_before = idpf_test_failures;

		/* Printed before the call so a hang names the guilty case. */
		printf("idpf_test:  -> %s.%s\n", suite->name,
		    suite->cases[i].name);

		idpf_test_current = suite->cases[i].name;
		suite->cases[i].fn();
		idpf_test_cases_run++;

		printf("idpf_test:  <- %s.%s %s\n", suite->name,
		    suite->cases[i].name,
		    idpf_test_failures == case_before ? "ok" : "FAIL");
	}
	idpf_test_current = "<none>";

	return (idpf_test_failures - before);
}

static const struct idpf_test_suite * const idpf_test_suites[] = {
	&idpf_test_suite_attach,
	&idpf_test_suite_ctlq,
};

static void
idpf_test_run_all(void)
{
	unsigned int i;

	printf("idpf_test: ==== start ====\n");
	for (i = 0; i < nitems(idpf_test_suites); i++)
		idpf_test_run_suite(idpf_test_suites[i]);

	printf("idpf_test: ==== done: cases=%d checks=%d failures=%d ====\n",
	    idpf_test_cases_run, idpf_test_checks, idpf_test_failures);
	printf("idpf_test: RESULT %s\n",
	    idpf_test_failures == 0 ? "PASS" : "FAIL");
}

/*
 * Loading must stay inert.  Running the suites from MOD_LOAD means a hang
 * cannot be unloaded and leaves no way to bisect which case wedged; the run is
 * triggered explicitly instead, one suite at a time if needed.
 */
static int
idpf_test_run_sysctl(SYSCTL_HANDLER_ARGS)
{
	unsigned int i;
	int which = -1;
	int err;

	err = sysctl_handle_int(oidp, &which, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	if (which < 0) {
		idpf_test_run_all();
		return (0);
	}
	if ((unsigned int)which >= nitems(idpf_test_suites))
		return (EINVAL);

	i = (unsigned int)which;
	printf("idpf_test: ==== start suite %u ====\n", i);
	idpf_test_run_suite(idpf_test_suites[i]);
	printf("idpf_test: RESULT %s\n",
	    idpf_test_failures == 0 ? "PASS" : "FAIL");

	return (0);
}

SYSCTL_PROC(_hw_idpf_test, OID_AUTO, run,
    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0, idpf_test_run_sysctl,
    "I", "write -1 for all suites, or a suite index to run just that one");

static int
idpf_test_modevent(module_t mod __unused, int type, void *arg __unused)
{

	switch (type) {
	case MOD_LOAD:
		printf("idpf_test: loaded (%zu suites); "
		    "run with sysctl hw.idpf_test.run=-1\n",
		    nitems(idpf_test_suites));
		return (0);
	case MOD_UNLOAD:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t idpf_test_mod = {
	"if_idpf_test", idpf_test_modevent, NULL
};

DECLARE_MODULE(if_idpf_test, idpf_test_mod, SI_SUB_PSEUDO, SI_ORDER_ANY);
MODULE_VERSION(if_idpf_test, 1);
MODULE_DEPEND(if_idpf_test, pci, 1, 1, 1);
MODULE_DEPEND(if_idpf_test, ether, 1, 1, 1);
MODULE_DEPEND(if_idpf_test, iflib, 1, 1, 1);
