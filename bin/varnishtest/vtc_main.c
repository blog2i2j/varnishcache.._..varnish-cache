/*-
 * Copyright (c) 2008-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "vtc.h"

#include "vev.h"
#include "vfil.h"
#include "vnum.h"
#include "vrnd.h"
#include "vss.h"
#include "vsub.h"
#include "vtcp.h"
#include "vtim.h"
#include "vct.h"

static const char *argv0;

struct buf {
	unsigned		magic;
#define BUF_MAGIC		0x39d1258a
	VTAILQ_ENTRY(buf)	list;
	char			*buf;
	struct vsb		*diag;
	size_t			bufsiz;
};

static VTAILQ_HEAD(, buf) free_bufs = VTAILQ_HEAD_INITIALIZER(free_bufs);

struct vtc_tst {
	unsigned		magic;
#define TST_MAGIC		0x618d8b88
	VTAILQ_ENTRY(vtc_tst)	list;
	const char		*filename;
	char			*script;
	unsigned		ntodo;
};

struct vtc_job {
	unsigned		magic;
#define JOB_MAGIC		0x1b5fc419
	struct vtc_tst		*tst;
	pid_t			child;
	struct vev		*ev;
	struct vev		*evt;
	struct buf		*bp;
	char			*tmpdir;
	double			t0;
	int			killed;
};


int iflg = 0;
unsigned vtc_maxdur = 60;
static unsigned vtc_bufsiz = 1024 * 1024;

static VTAILQ_HEAD(, vtc_tst) tst_head = VTAILQ_HEAD_INITIALIZER(tst_head);
static struct vev_root *vb;
static int njob = 0;
static int npar = 1;			/* Number of parallel tests */
static int vtc_continue;		/* Continue on error */
static int vtc_verbosity = 1;		/* Verbosity Level */
static int vtc_good;
static int vtc_fail;
static int vtc_skip;
static char *tmppath;
static char *cwd = NULL;
char *vmod_path = NULL;
struct vsb *params_vsb = NULL;
int leave_temp;
int vtc_witness = 0;
static struct vsb *cbvsb;
static int bad_backend_fd;

static int cleaner_fd = -1;
static pid_t cleaner_pid;

static struct buf *
get_buf(void)
{
	struct buf *bp;

	bp = VTAILQ_FIRST(&free_bufs);
	CHECK_OBJ_ORNULL(bp, BUF_MAGIC);
	if (bp != NULL) {
		VTAILQ_REMOVE(&free_bufs, bp, list);
		VSB_clear(bp->diag);
	} else {
		ALLOC_OBJ(bp, BUF_MAGIC);
		AN(bp);
		bp->bufsiz = vtc_bufsiz;
		bp->buf = mmap(NULL, bp->bufsiz, PROT_READ|PROT_WRITE,
		    MAP_ANON | MAP_SHARED, -1, 0);
		assert(bp->buf != MAP_FAILED);
		bp->diag = VSB_new_auto();
		AN(bp->diag);
	}
	memset(bp->buf, 0, bp->bufsiz);
	return (bp);
}

static void
rel_buf(struct buf **bp)
{
	CHECK_OBJ_NOTNULL(*bp, BUF_MAGIC);

	VTAILQ_INSERT_HEAD(&free_bufs, (*bp), list);
	*bp = NULL;
}

/**********************************************************************
 * Parse a -D option argument into a name/val pair, and insert
 * into extmacro list
 */

static int
parse_D_opt(char *arg)
{
	char *p, *q;

	p = arg;
	q = strchr(p, '=');
	if (!q)
		return (0);
	*q++ = '\0';
	extmacro_def(p, "%s", q);

	return (1);
}

/**********************************************************************
 * Print usage
 */

static void
usage(void)
{
	fprintf(stderr, "usage: %s [options] file ...\n", argv0);
#define FMT "    %-28s # %s\n"
	fprintf(stderr, FMT, "-b size",
	    "Set internal buffer size (default: 1M)");
	fprintf(stderr, FMT, "-C", "Use cleaner subprocess");
	fprintf(stderr, FMT, "-D name=val", "Define macro");
	fprintf(stderr, FMT, "-i", "Find varnish binaries in build tree");
	fprintf(stderr, FMT, "-j jobs", "Run this many tests in parallel");
	fprintf(stderr, FMT, "-k", "Continue on test failure");
	fprintf(stderr, FMT, "-L", "Always leave temporary vtc.*");
	fprintf(stderr, FMT, "-l", "Leave temporary vtc.* if test fails");
	fprintf(stderr, FMT, "-n iterations", "Run tests this many times");
	fprintf(stderr, FMT, "-p name=val", "Pass a varnishd parameter");
	fprintf(stderr, FMT, "-q", "Quiet mode: report only failures");
	fprintf(stderr, FMT, "-t duration", "Time tests out after this long");
	fprintf(stderr, FMT, "-v", "Verbose mode: always report test log");
	fprintf(stderr, FMT, "-W", "Enable the witness facility for locking");
	exit(1);
}

/**********************************************************************
 * When running many tests, cleaning the tmpdir with "rm -rf" becomes
 * chore which limits our performance.
 * When the number of tests are above 100, we spawn a child-process
 * to do that for us.
 */

static void
cleaner_do(const char *dirname)
{
	char buf[BUFSIZ];

	AZ(memcmp(dirname, tmppath, strlen(tmppath)));
	if (cleaner_pid > 0) {
		bprintf(buf, "%s\n", dirname);
		assert(write(cleaner_fd, buf, strlen(buf)) == strlen(buf));
		return;
	}
	bprintf(buf, "exec /bin/rm -rf %s\n", dirname);
	AZ(system(buf));
}

static void
cleaner_setup(void)
{
	int p[2], st;
	char buf[BUFSIZ];
	char *q;
	pid_t pp;

	AZ(pipe(p));
	assert(p[0] > STDERR_FILENO);
	assert(p[1] > STDERR_FILENO);
	cleaner_pid = fork();
	assert(cleaner_pid >= 0);
	if (cleaner_pid == 0) {
		closefd(&p[1]);
		(void)nice(1);		/* Not important */
		setbuf(stdin, NULL);
		AZ(dup2(p[0], STDIN_FILENO));
		while (fgets(buf, sizeof buf, stdin)) {
			AZ(memcmp(buf, tmppath, strlen(tmppath)));
			q = buf + strlen(buf);
			assert(q > buf);
			assert(q[-1] == '\n');
			q[-1] = '\0';

			/* Dont expend a shell on running /bin/rm */
			pp = fork();
			assert(pp >= 0);
			if (pp == 0)
				exit(execl(
				    "/bin/rm", "rm", "-rf", buf, (char*)0));
			assert(waitpid(pp, &st, 0) == pp);
			AZ(st);
		}
		exit(0);
	}
	closefd(&p[0]);
	cleaner_fd = p[1];
}

static void
cleaner_neuter(void)
{
	if (cleaner_pid > 0)
		closefd(&cleaner_fd);
}

static void
cleaner_finish(void)
{
	int st;

	if (cleaner_pid > 0) {
		closefd(&cleaner_fd);
		assert(waitpid(cleaner_pid, &st, 0) == cleaner_pid);
		AZ(st);
	}
}

/**********************************************************************
 * CallBack
 */

static int
tst_cb(const struct vev *ve, int what)
{
	struct vtc_job *jp;
	char buf[BUFSIZ];
	int ecode;
	int i, stx;
	pid_t px;
	double t;
	FILE *f;
	char *p;

	CAST_OBJ_NOTNULL(jp, ve->priv, JOB_MAGIC);

	// printf("CB %p %s %d\n", ve, jp->tst->filename, what);
	if (what == 0) {
		jp->killed = 1;
		AZ(kill(-jp->child, SIGKILL)); /* XXX: Timeout */
	} else {
		assert(what & (VEV__RD | VEV__HUP));
	}

	*buf = '\0';
	i = read(ve->fd, buf, sizeof buf);
	if (i > 0)
		VSB_bcat(jp->bp->diag, buf, i);
	if (i == 0) {

		njob--;
		px = wait4(jp->child, &stx, 0, NULL);
		assert(px == jp->child);
		t = VTIM_mono() - jp->t0;
		AZ(close(ve->fd));

		ecode = WTERMSIG(stx);
		if (ecode == 0)
			ecode = WEXITSTATUS(stx);

		AZ(VSB_finish(jp->bp->diag));

		VSB_clear(cbvsb);
		VSB_cat(cbvsb, jp->bp->buf);
		p = strchr(jp->bp->buf, '\0');
		if (p > jp->bp->buf && p[-1] != '\n')
			VSB_putc(cbvsb, '\n');
		VSB_quote_pfx(cbvsb, "*    diag  0.0 ",
		    VSB_data(jp->bp->diag), -1, VSB_QUOTE_NONL);
		AZ(VSB_finish(cbvsb));
		rel_buf(&jp->bp);

		if ((ecode > 1 && vtc_verbosity) || vtc_verbosity > 1)
			printf("%s", VSB_data(cbvsb));

		if (!ecode)
			vtc_good++;
		else if (ecode == 1)
			vtc_skip++;
		else
			vtc_fail++;

		if (leave_temp == 0 || (leave_temp == 1 && ecode <= 1)) {
			cleaner_do(jp->tmpdir);
		} else {
			bprintf(buf, "%s/LOG", jp->tmpdir);
			f = fopen(buf, "w");
			AN(f);
			(void)fprintf(f, "%s\n", VSB_data(cbvsb));
			AZ(fclose(f));
		}
		free(jp->tmpdir);

		if (jp->killed)
			printf("#    top  TEST %s TIMED OUT (kill -9)\n",
			    jp->tst->filename);
		if (ecode > 1) {
			printf("#    top  TEST %s FAILED (%.3f)",
			    jp->tst->filename, t);
			if (WIFSIGNALED(stx))
				printf(" signal=%d\n", WTERMSIG(stx));
			else if (WIFEXITED(stx))
				printf(" exit=%d\n", WEXITSTATUS(stx));
			if (!vtc_continue) {
				/* XXX kill -9 other jobs ? */
				exit(2);
			}
		} else if (vtc_verbosity) {
			printf("#    top  TEST %s %s (%.3f)\n",
			    jp->tst->filename,
			    ecode ? "skipped" : "passed", t);
		}
		if (jp->evt != NULL) {
			VEV_Stop(vb, jp->evt);
			free(jp->evt);
		}
		FREE_OBJ(jp);
		return (1);
	}
	return (0);
}

/**********************************************************************
 * Start Test
 */

static void
start_test(void)
{
	struct vtc_tst *tp;
	int p[2], retval;
	struct vtc_job *jp;
	char tmpdir[PATH_MAX];

	ALLOC_OBJ(jp, JOB_MAGIC);
	AN(jp);

	jp->bp = get_buf();

	bprintf(tmpdir, "%s/vtc.%d.%08x", tmppath, (int)getpid(),
		(unsigned)random());
	AZ(mkdir(tmpdir, 0711));

	tp = VTAILQ_FIRST(&tst_head);
	CHECK_OBJ_NOTNULL(tp, TST_MAGIC);
	AN(tp->ntodo);
	tp->ntodo--;
	VTAILQ_REMOVE(&tst_head, tp, list);
	if (tp->ntodo > 0)
		VTAILQ_INSERT_TAIL(&tst_head, tp, list);

	jp->tst = tp;
	jp->tmpdir = strdup(tmpdir);
	AN(jp->tmpdir);

	AZ(pipe(p));
	assert(p[0] > STDERR_FILENO);
	assert(p[1] > STDERR_FILENO);
	jp->t0 = VTIM_mono();
	jp->child = fork();
	assert(jp->child >= 0);
	if (jp->child == 0) {
		cleaner_neuter();	// Too dangerous to have around
		AZ(setpgid(getpid(), 0));
		VFIL_null_fd(STDIN_FILENO);
		assert(dup2(p[1], STDOUT_FILENO) == STDOUT_FILENO);
		assert(dup2(p[1], STDERR_FILENO) == STDERR_FILENO);
		VSUB_closefrom(STDERR_FILENO + 1);
		retval = exec_file(jp->tst->filename, jp->tst->script,
		    jp->tmpdir, jp->bp->buf, jp->bp->bufsiz);
		exit(retval);
	}
	closefd(&p[1]);

	jp->ev = VEV_Alloc();
	AN(jp->ev);
	jp->ev->fd_flags = VEV__RD | VEV__HUP | VEV__ERR;
	jp->ev->fd = p[0];
	jp->ev->priv = jp;
	jp->ev->callback = tst_cb;
	AZ(VEV_Start(vb, jp->ev));

	jp->evt = VEV_Alloc();
	AN(jp->evt);
	jp->evt->fd = -1;
	jp->evt->timeout = vtc_maxdur;
	jp->evt->priv = jp;
	jp->evt->callback = tst_cb;
	AZ(VEV_Start(vb, jp->evt));
}

/**********************************************************************
 * i-mode = "we're inside a src-tree"
 *
 * Find the abs path to top of source dir from Makefile, if that
 * fails, fall back on "../../"
 *
 * Set path to all programs build directories
 *
 */

static void
i_mode(void)
{
	const char *sep;
	struct vsb *vsb;
	char *p, *q;
	char *topbuild;

	/*
	 * This code has a rather intimate knowledge of auto* generated
	 * makefiles.
	 */

	vsb = VSB_new_auto();
	AN(vsb);

	q = p = VFIL_readfile(NULL, "Makefile", NULL);
	if (p == NULL) {
		fprintf(stderr, "No Makefile to search for -i flag.\n");
		VSB_printf(vsb, "%s/../..", cwd);
		AZ(VSB_finish(vsb));
		topbuild = strdup(VSB_data(vsb));
		VSB_clear(vsb);
	} else {
		p = strstr(p, "\nabs_top_builddir");
		if (p == NULL) {
			fprintf(stderr,
			    "could not find 'abs_top_builddir' in Makefile\n");
			exit(2);
		}
		topbuild =  strchr(p + 1, '\n');
		if (topbuild == NULL) {
			fprintf(stderr,
			    "No NL after 'abs_top_builddir' in Makefile\n");
			exit(2);
		}
		*topbuild = '\0';
		topbuild = strchr(p, '/');
		if (topbuild == NULL) {
			fprintf(stderr,
			    "No '/' after 'abs_top_builddir' in Makefile\n");
			exit(2);
		}
		topbuild = strdup(topbuild);
		free(q);

	}
	AN(topbuild);
	extmacro_def("topbuild", "%s", topbuild);
	/*
	 * Build $PATH which can find all programs in the build tree
	 */
	VSB_printf(vsb, "PATH=");
	sep = "";
#define VTC_PROG(l)							\
	do {								\
		VSB_printf(vsb, "%s%s/bin/" #l, sep, topbuild);		\
		sep = ":";						\
	} while (0);
#include "programs.h"

	VSB_printf(vsb, ":%s", getenv("PATH"));
	AZ(VSB_finish(vsb));

	AZ(putenv(strdup(VSB_data(vsb))));

	/*
	 * Build vmod_path which can find all VMODs in the build tree
	 */
	VSB_clear(vsb);
	sep = "";
#define VTC_VMOD(l)							\
	do {								\
		VSB_printf(vsb, "%s%s/lib/libvmod_" #l "/.libs",	\
		    sep, topbuild);					\
		sep = ":";						\
	} while (0);
#include "vmods.h"
#undef VTC_VMOD
	AZ(VSB_finish(vsb));
	vmod_path = strdup(VSB_data(vsb));
	AN(vmod_path);
	free(topbuild);
	VSB_destroy(&vsb);

	/*
	 * strict jemalloc checking
	 */
	AZ(putenv(strdup("MALLOC_CONF=abort:true,junk:true")));
}

/**********************************************************************
 * Figure out what IP related magic
 */

static void
ip_magic(void)
{
	struct suckaddr *sa;
	char abuf[VTCP_ADDRBUFSIZE];
	char pbuf[VTCP_PORTBUFSIZE];

	/*
	 * In FreeBSD jails localhost/127.0.0.1 becomes the jails IP#
	 * XXX: IPv6-only hosts would have similar issue, but it is not
	 * XXX: obvious how to cope.  Ideally "127.0.0.1" would be
	 * XXX: "localhost", but that doesn't work out of the box.
	 * XXX: Things like "prefer_ipv6" parameter complicates things.
	 */
	sa = VSS_ResolveOne(NULL, "127.0.0.1", "0", 0, SOCK_STREAM, 0);
	AN(sa);
	bad_backend_fd = VTCP_bind(sa, NULL);
	free(sa);
	assert(bad_backend_fd >= 0);
	VTCP_myname(bad_backend_fd, abuf, sizeof abuf, pbuf, sizeof(pbuf));
	extmacro_def("localhost", "%s", abuf);

#if defined (__APPLE__)
	/*
	 * In MacOS a bound socket that is not listening will timeout
	 * instead of refusing the connection so close it and hope
	 * for the best.
	 */
	VTCP_close(&bad_backend_fd);
#endif

	/* Expose a backend that is forever down. */
	extmacro_def("bad_backend", "%s %s", abuf, pbuf);

	/*
	 * We need an IP number which will not repond, ever, and that is a
	 * lot harder than it sounds.  This IP# is from RFC5737 and a
	 * C-class broadcast at that.
	 * If tests involving ${bad_ip} fails and you run linux, you should
	 * check your /proc/sys/net/ipv4/ip_nonlocal_bind setting.
	 */

	extmacro_def("bad_ip", "%s", "192.0.2.255");
}

/**********************************************************************
 * Main
 */

static int
read_file(const char *fn, int ntest)
{
	struct vtc_tst *tp;
	char *p, *q;

	p = VFIL_readfile(NULL, fn, NULL);
	if (p == NULL) {
		fprintf(stderr, "Cannot stat file \"%s\": %s\n",
		    fn, strerror(errno));
		return (2);
	}
	for (q = p ;q != NULL && *q != '\0'; q++) {
		if (vct_islws(*q))
			continue;
		if (*q != '#')
			break;
		q = strchr(q, '\n');
		if (q == NULL)
			break;
	}

	if (q == NULL || *q == '\0') {
		fprintf(stderr, "File \"%s\" has no content.\n", fn);
		free(p);
		return (2);
	}

	if ((strncmp(q, "varnishtest", 11) || !isspace(q[11])) &&
	    (strncmp(q, "vtest", 5) || !isspace(q[5]))) {
		fprintf(stderr,
		    "File \"%s\" doesn't start with"
		    " 'vtest' or 'varnishtest'\n", fn);
		free(p);
		vtc_skip++;
		return (2);
	}
	ALLOC_OBJ(tp, TST_MAGIC);
	AN(tp);
	tp->filename = fn;
	tp->script = p;
	tp->ntodo = ntest;
	VTAILQ_INSERT_TAIL(&tst_head, tp, list);
	return (0);
}

/**********************************************************************
 * Main
 */

int
main(int argc, char * const *argv)
{
	int ch, i;
	int ntest = 1;			/* Run tests this many times */
	int nstart = 0;
	int use_cleaner = 0;
	uintmax_t bufsiz;
	const char *p;

	argv0 = strrchr(argv[0], '/');
	if (argv0 == NULL)
		argv0 = argv[0];
	else
		argv0++;

	if (getenv("TMPDIR") != NULL)
		tmppath = strdup(getenv("TMPDIR"));
	else
		tmppath = strdup("/tmp");

	cwd = getcwd(NULL, PATH_MAX);
	extmacro_def("pwd", "%s", cwd);

	vmod_path = NULL;

	params_vsb = VSB_new_auto();
	AN(params_vsb);
	p = getenv("VTEST_DURATION");
	if (p == NULL)
		p = getenv("VARNISHTEST_DURATION");
	if (p != NULL)
		vtc_maxdur = atoi(p);

	VRND_SeedAll();
	cbvsb = VSB_new_auto();
	AN(cbvsb);
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	while ((ch = getopt(argc, argv, "b:CD:hij:kLln:p:qt:vW")) != -1) {
		switch (ch) {
		case 'b':
			if (VNUM_2bytes(optarg, &bufsiz, 0)) {
				fprintf(stderr, "Cannot parse b opt '%s'\n",
				    optarg);
				exit(2);
			}
			if (bufsiz > UINT_MAX) {
				fprintf(stderr, "Invalid b opt '%s'\n",
				    optarg);
				exit(2);
			}
			vtc_bufsiz = (unsigned)bufsiz;
			break;
		case 'C':
			use_cleaner = !use_cleaner;
			break;
		case 'D':
			if (!parse_D_opt(optarg)) {
				fprintf(stderr, "Cannot parse D opt '%s'\n",
				    optarg);
				exit(2);
			}
			break;
		case 'i':
			iflg = 1;
			break;
		case 'j':
			npar = strtoul(optarg, NULL, 0);
			break;
		case 'L':
			leave_temp = 2;
			break;
		case 'l':
			leave_temp = 1;
			break;
		case 'k':
			vtc_continue = !vtc_continue;
			break;
		case 'n':
			ntest = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			VSB_printf(params_vsb, " -p ");
			VSB_quote(params_vsb, optarg, -1, 0);
			break;
		case 'q':
			if (vtc_verbosity > 0)
				vtc_verbosity--;
			break;
		case 't':
			vtc_maxdur = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			if (vtc_verbosity < 2)
				vtc_verbosity++;
			break;
		case 'W':
			vtc_witness++;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1)
		usage();

	for (; argc > 0; argc--, argv++) {
		if (!read_file(*argv, ntest))
			continue;
		if (!vtc_continue)
			exit(2);
	}

	AZ(VSB_finish(params_vsb));

	ip_magic();

	if (iflg)
		i_mode();

	vb = VEV_New();

	if (use_cleaner)
		cleaner_setup();

	i = 0;
	while (!VTAILQ_EMPTY(&tst_head) || i) {
		if (!VTAILQ_EMPTY(&tst_head) && njob < npar) {
			start_test();
			njob++;
			/* Stagger ramp-up */
			if (nstart++ < npar)
				(void)usleep(random() % 100000L);
			i = 1;
			continue;
		}
		i = VEV_Once(vb);
	}
	cleaner_finish();
	(void)close(bad_backend_fd);
	if (vtc_continue)
		fprintf(stderr,
		    "%d tests failed, %d tests skipped, %d tests passed\n",
		    vtc_fail, vtc_skip, vtc_good);
	if (vtc_fail)
		return (1);
	if (vtc_skip && !vtc_good)
		return (77);
	return (0);
}
