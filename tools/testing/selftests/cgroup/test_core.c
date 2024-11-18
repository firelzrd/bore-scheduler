/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE
#include <linux/limits.h>
#include <linux/sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <errno.h>

#include "../kselftest.h"
#include "cgroup_util.h"

/*
 * A(0) - B(0) - C(1)
 *        \ D(0)
 *
 * A, B and C's "populated" fields would be 1 while D's 0.
 * test that after the one process in C is moved to root,
 * A,B and C's "populated" fields would flip to "0" and file
 * modified events will be generated on the
 * "cgroup.events" files of both cgroups.
 */
static int test_cgcore_populated(const char *root)
{
	int ret = KSFT_FAIL;
	char *cg_test_a = NULL, *cg_test_b = NULL;
	char *cg_test_c = NULL, *cg_test_d = NULL;

	cg_test_a = cg_name(root, "cg_test_a");
	cg_test_b = cg_name(root, "cg_test_a/cg_test_b");
	cg_test_c = cg_name(root, "cg_test_a/cg_test_b/cg_test_c");
	cg_test_d = cg_name(root, "cg_test_a/cg_test_b/cg_test_d");

	if (!cg_test_a || !cg_test_b || !cg_test_c || !cg_test_d)
		goto cleanup;

	if (cg_create(cg_test_a))
		goto cleanup;

	if (cg_create(cg_test_b))
		goto cleanup;

	if (cg_create(cg_test_c))
		goto cleanup;

	if (cg_create(cg_test_d))
		goto cleanup;

	if (cg_enter_current(cg_test_c))
		goto cleanup;

	if (cg_read_strcmp(cg_test_a, "cgroup.events", "populated 1\n"))
		goto cleanup;

	if (cg_read_strcmp(cg_test_b, "cgroup.events", "populated 1\n"))
		goto cleanup;

	if (cg_read_strcmp(cg_test_c, "cgroup.events", "populated 1\n"))
		goto cleanup;

	if (cg_read_strcmp(cg_test_d, "cgroup.events", "populated 0\n"))
		goto cleanup;

	if (cg_enter_current(root))
		goto cleanup;

	if (cg_read_strcmp(cg_test_a, "cgroup.events", "populated 0\n"))
		goto cleanup;

	if (cg_read_strcmp(cg_test_b, "cgroup.events", "populated 0\n"))
		goto cleanup;

	if (cg_read_strcmp(cg_test_c, "cgroup.events", "populated 0\n"))
		goto cleanup;

	if (cg_read_strcmp(cg_test_d, "cgroup.events", "populated 0\n"))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	if (cg_test_d)
		cg_destroy(cg_test_d);
	if (cg_test_c)
		cg_destroy(cg_test_c);
	if (cg_test_b)
		cg_destroy(cg_test_b);
	if (cg_test_a)
		cg_destroy(cg_test_a);
	free(cg_test_d);
	free(cg_test_c);
	free(cg_test_b);
	free(cg_test_a);
	return ret;
}

/*
 * A (domain threaded) - B (threaded) - C (domain)
 *
 * test that C can't be used until it is turned into a
 * threaded cgroup.  "cgroup.type" file will report "domain (invalid)" in
 * these cases. Operations which fail due to invalid topology use
 * EOPNOTSUPP as the errno.
 */
static int test_cgcore_invalid_domain(const char *root)
{
	int ret = KSFT_FAIL;
	char *grandparent = NULL, *parent = NULL, *child = NULL;

	grandparent = cg_name(root, "cg_test_grandparent");
	parent = cg_name(root, "cg_test_grandparent/cg_test_parent");
	child = cg_name(root, "cg_test_grandparent/cg_test_parent/cg_test_child");
	if (!parent || !child || !grandparent)
		goto cleanup;

	if (cg_create(grandparent))
		goto cleanup;

	if (cg_create(parent))
		goto cleanup;

	if (cg_create(child))
		goto cleanup;

	if (cg_write(parent, "cgroup.type", "threaded"))
		goto cleanup;

	if (cg_read_strcmp(child, "cgroup.type", "domain invalid\n"))
		goto cleanup;

	if (!cg_enter_current(child))
		goto cleanup;

	if (errno != EOPNOTSUPP)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_enter_current(root);
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	if (grandparent)
		cg_destroy(grandparent);
	free(child);
	free(parent);
	free(grandparent);
	return ret;
}

/*
 * Test that when a child becomes threaded
 * the parent type becomes domain threaded.
 */
static int test_cgcore_parent_becomes_threaded(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent = NULL, *child = NULL;

	parent = cg_name(root, "cg_test_parent");
	child = cg_name(root, "cg_test_parent/cg_test_child");
	if (!parent || !child)
		goto cleanup;

	if (cg_create(parent))
		goto cleanup;

	if (cg_create(child))
		goto cleanup;

	if (cg_write(child, "cgroup.type", "threaded"))
		goto cleanup;

	if (cg_read_strcmp(parent, "cgroup.type", "domain threaded\n"))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	free(child);
	free(parent);
	return ret;

}

/*
 * Test that there's no internal process constrain on threaded cgroups.
 * You can add threads/processes on a parent with a controller enabled.
 */
static int test_cgcore_no_internal_process_constraint_on_threads(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent = NULL, *child = NULL;

	if (cg_read_strstr(root, "cgroup.controllers", "cpu") ||
	    cg_write(root, "cgroup.subtree_control", "+cpu")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	parent = cg_name(root, "cg_test_parent");
	child = cg_name(root, "cg_test_parent/cg_test_child");
	if (!parent || !child)
		goto cleanup;

	if (cg_create(parent))
		goto cleanup;

	if (cg_create(child))
		goto cleanup;

	if (cg_write(parent, "cgroup.type", "threaded"))
		goto cleanup;

	if (cg_write(child, "cgroup.type", "threaded"))
		goto cleanup;

	if (cg_write(parent, "cgroup.subtree_control", "+cpu"))
		goto cleanup;

	if (cg_enter_current(parent))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_enter_current(root);
	cg_enter_current(root);
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	free(child);
	free(parent);
	return ret;
}

/*
 * Test that you can't enable a controller on a child if it's not enabled
 * on the parent.
 */
static int test_cgcore_top_down_constraint_enable(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent = NULL, *child = NULL;

	parent = cg_name(root, "cg_test_parent");
	child = cg_name(root, "cg_test_parent/cg_test_child");
	if (!parent || !child)
		goto cleanup;

	if (cg_create(parent))
		goto cleanup;

	if (cg_create(child))
		goto cleanup;

	if (!cg_write(child, "cgroup.subtree_control", "+memory"))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	free(child);
	free(parent);
	return ret;
}

/*
 * Test that you can't disable a controller on a parent
 * if it's enabled in a child.
 */
static int test_cgcore_top_down_constraint_disable(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent = NULL, *child = NULL;

	parent = cg_name(root, "cg_test_parent");
	child = cg_name(root, "cg_test_parent/cg_test_child");
	if (!parent || !child)
		goto cleanup;

	if (cg_create(parent))
		goto cleanup;

	if (cg_create(child))
		goto cleanup;

	if (cg_write(parent, "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (cg_write(child, "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (!cg_write(parent, "cgroup.subtree_control", "-memory"))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	free(child);
	free(parent);
	return ret;
}

/*
 * Test internal process constraint.
 * You can't add a pid to a domain parent if a controller is enabled.
 */
static int test_cgcore_internal_process_constraint(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent = NULL, *child = NULL;

	parent = cg_name(root, "cg_test_parent");
	child = cg_name(root, "cg_test_parent/cg_test_child");
	if (!parent || !child)
		goto cleanup;

	if (cg_create(parent))
		goto cleanup;

	if (cg_create(child))
		goto cleanup;

	if (cg_write(parent, "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (!cg_enter_current(parent))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	if (child)
		cg_destroy(child);
	if (parent)
		cg_destroy(parent);
	free(child);
	free(parent);
	return ret;
}

/*
 * cgroup migration permission check should be performed based on the
 * credentials at the time of open instead of write.
 */
static int test_cgcore_lesser_euid_open(const char *root)
{
	const uid_t test_euid = 65534;	/* usually nobody, any !root is fine */
	int ret = KSFT_FAIL;
	char *cg_test_a = NULL, *cg_test_b = NULL;
	char *cg_test_a_procs = NULL, *cg_test_b_procs = NULL;
	int cg_test_b_procs_fd = -1;
	uid_t saved_uid;

	cg_test_a = cg_name(root, "cg_test_a");
	cg_test_b = cg_name(root, "cg_test_b");

	if (!cg_test_a || !cg_test_b)
		goto cleanup;

	cg_test_a_procs = cg_name(cg_test_a, "cgroup.procs");
	cg_test_b_procs = cg_name(cg_test_b, "cgroup.procs");

	if (!cg_test_a_procs || !cg_test_b_procs)
		goto cleanup;

	if (cg_create(cg_test_a) || cg_create(cg_test_b))
		goto cleanup;

	if (cg_enter_current(cg_test_a))
		goto cleanup;

	if (chown(cg_test_a_procs, test_euid, -1) ||
	    chown(cg_test_b_procs, test_euid, -1))
		goto cleanup;

	saved_uid = geteuid();
	if (seteuid(test_euid))
		goto cleanup;

	cg_test_b_procs_fd = open(cg_test_b_procs, O_RDWR);

	if (seteuid(saved_uid))
		goto cleanup;

	if (cg_test_b_procs_fd < 0)
		goto cleanup;

	if (write(cg_test_b_procs_fd, "0", 1) >= 0 || errno != EACCES)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_enter_current(root);
	if (cg_test_b_procs_fd >= 0)
		close(cg_test_b_procs_fd);
	if (cg_test_b)
		cg_destroy(cg_test_b);
	if (cg_test_a)
		cg_destroy(cg_test_a);
	free(cg_test_b_procs);
	free(cg_test_a_procs);
	free(cg_test_b);
	free(cg_test_a);
	return ret;
}

struct lesser_ns_open_thread_arg {
	const char	*path;
	int		fd;
	int		err;
};

static int lesser_ns_open_thread_fn(void *arg)
{
	struct lesser_ns_open_thread_arg *targ = arg;

	targ->fd = open(targ->path, O_RDWR);
	targ->err = errno;
	return 0;
}

/*
 * cgroup migration permission check should be performed based on the cgroup
 * namespace at the time of open instead of write.
 */
static int test_cgcore_lesser_ns_open(const char *root)
{
	static char stack[65536];
	const uid_t test_euid = 65534;	/* usually nobody, any !root is fine */
	int ret = KSFT_FAIL;
	char *cg_test_a = NULL, *cg_test_b = NULL;
	char *cg_test_a_procs = NULL, *cg_test_b_procs = NULL;
	int cg_test_b_procs_fd = -1;
	struct lesser_ns_open_thread_arg targ = { .fd = -1 };
	pid_t pid;
	int status;

	cg_test_a = cg_name(root, "cg_test_a");
	cg_test_b = cg_name(root, "cg_test_b");

	if (!cg_test_a || !cg_test_b)
		goto cleanup;

	cg_test_a_procs = cg_name(cg_test_a, "cgroup.procs");
	cg_test_b_procs = cg_name(cg_test_b, "cgroup.procs");

	if (!cg_test_a_procs || !cg_test_b_procs)
		goto cleanup;

	if (cg_create(cg_test_a) || cg_create(cg_test_b))
		goto cleanup;

	if (cg_enter_current(cg_test_b))
		goto cleanup;

	if (chown(cg_test_a_procs, test_euid, -1) ||
	    chown(cg_test_b_procs, test_euid, -1))
		goto cleanup;

	targ.path = cg_test_b_procs;
	pid = clone(lesser_ns_open_thread_fn, stack + sizeof(stack),
		    CLONE_NEWCGROUP | CLONE_FILES | CLONE_VM | SIGCHLD,
		    &targ);
	if (pid < 0)
		goto cleanup;

	if (waitpid(pid, &status, 0) < 0)
		goto cleanup;

	if (!WIFEXITED(status))
		goto cleanup;

	cg_test_b_procs_fd = targ.fd;
	if (cg_test_b_procs_fd < 0)
		goto cleanup;

	if (cg_enter_current(cg_test_a))
		goto cleanup;

	if ((status = write(cg_test_b_procs_fd, "0", 1)) >= 0 || errno != ENOENT)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_enter_current(root);
	if (cg_test_b_procs_fd >= 0)
		close(cg_test_b_procs_fd);
	if (cg_test_b)
		cg_destroy(cg_test_b);
	if (cg_test_a)
		cg_destroy(cg_test_a);
	free(cg_test_b_procs);
	free(cg_test_a_procs);
	free(cg_test_b);
	free(cg_test_a);
	return ret;
}

#define T(x) { x, #x }
struct corecg_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_cgcore_internal_process_constraint),
	T(test_cgcore_top_down_constraint_enable),
	T(test_cgcore_top_down_constraint_disable),
	T(test_cgcore_no_internal_process_constraint_on_threads),
	T(test_cgcore_parent_becomes_threaded),
	T(test_cgcore_invalid_domain),
	T(test_cgcore_populated),
	T(test_cgcore_lesser_euid_open),
	T(test_cgcore_lesser_ns_open),
};
#undef T

int main(int argc, char *argv[])
{
	char root[PATH_MAX];
	int i, ret = EXIT_SUCCESS;

	if (cg_find_unified_root(root, sizeof(root)))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	if (cg_read_strstr(root, "cgroup.subtree_control", "memory"))
		if (cg_write(root, "cgroup.subtree_control", "+memory"))
			ksft_exit_skip("Failed to set memory controller\n");

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ret = EXIT_FAILURE;
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	return ret;
}
