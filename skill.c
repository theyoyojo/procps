/*
 * Copyright 1998-2002 by Albert Cahalan; all rights resered.
 * This file may be used subject to the terms and conditions of the
 * GNU Library General Public License Version 2, or any later version
 * at your option, as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Library General Public License for more details.
 */
#include <dirent.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "c.h"
#include "strutils.h"
#include "nls.h"
#include "xalloc.h"
#include "proc/pwcache.h"
#include "proc/sig.h"
#include "proc/devname.h"
#include "proc/procps.h"	/* char *user_from_uid(uid_t uid) */
#include "proc/version.h"	/* procps_version */

#define DEFAULT_NICE 4

struct run_time_conf_t {
	int fast;
	int interactive;
	int verbose;
	int warnings;
	int noaction;
	int debugging;
};
static int tty_count, uid_count, cmd_count, pid_count;
static int *ttys;
static uid_t *uids;
static const char **cmds;
static int *pids;

#define ENLIST(thing,addme) do{ \
if(!thing##s) thing##s = xmalloc(sizeof(*thing##s)*saved_argc); \
thing##s[thing##_count++] = addme; \
}while(0)

static int my_pid;
static int saved_argc;

static int sig_or_pri;

enum {
	PROG_UNKNOWN,
	PROG_KILL,
	PROG_SKILL,
	PROG_SNICE
};
static int program = PROG_UNKNOWN;

static void display_kill_version(void)
{
	fprintf(stdout, PROCPS_NG_VERSION);
}

/* kill or nice a process */
static void hurt_proc(int tty, int uid, int pid, const char *restrict const cmd,
		      struct run_time_conf_t *run_time)
{
	int failed;
	int saved_errno;
	char dn_buf[1000];
	dev_to_tty(dn_buf, 999, tty, pid, ABBREV_DEV);
	if (run_time->interactive) {
		char buf[8];
		fprintf(stderr, "%-8s %-8s %5d %-16.16s   ? ",
			(char *)dn_buf, user_from_uid(uid), pid, cmd);
		if (!fgets(buf, 7, stdin)) {
			printf("\n");
			exit(0);
		}
		if (*buf != 'y' && *buf != 'Y')
			return;
	}
	/* do the actual work */
	if (program == PROG_SKILL)
		failed = kill(pid, sig_or_pri);
	else
		failed = setpriority(PRIO_PROCESS, pid, sig_or_pri);
	saved_errno = errno;
	if ((run_time->warnings && failed) || run_time->debugging) {
		fprintf(stderr, "%-8s %-8s %5d %-16.16s   ",
			(char *)dn_buf, user_from_uid(uid), pid, cmd);
		errno = saved_errno;
		perror("");
		return;
	}
	if (run_time->interactive)
		return;
	if (run_time->verbose) {
		printf("%-8s %-8s %5d %-16.16s\n",
		       (char *)dn_buf, user_from_uid(uid), pid, cmd);
		return;
	}
	if (run_time->noaction) {
		printf("%d\n", pid);
		return;
	}
}

/* check one process */
static void check_proc(int pid, struct run_time_conf_t *run_time)
{
	char buf[128];
	struct stat statbuf;
	char *tmp;
	int tty;
	int fd;
	int i;
	if (pid == my_pid || pid == 0)
		return;
	/* pid (cmd) state ppid pgrp session tty */
	sprintf(buf, "/proc/%d/stat", pid);
	fd = open(buf, O_RDONLY);
	if (fd == -1) {
		/* process exited maybe */
		if (run_time->warnings)
			warn(_("cannot open file %s"), buf);
		return;
	}
	fstat(fd, &statbuf);
	if (uids) {
		/* check the EUID */
		i = uid_count;
		while (i--)
			if (uids[i] == statbuf.st_uid)
				break;
		if (i == -1)
			goto closure;
	}
	read(fd, buf, 128);
	buf[127] = '\0';
	tmp = strrchr(buf, ')');
	*tmp++ = '\0';
	i = 5;
	while (i--)
		while (*tmp++ != ' ')
			/* scan to find tty */ ;
	tty = atoi(tmp);
	if (ttys) {
		i = tty_count;
		while (i--)
			if (ttys[i] == tty)
				break;
		if (i == -1)
			goto closure;
	}
	tmp = strchr(buf, '(') + 1;
	if (cmds) {
		i = cmd_count;
		/* fast comparison trick -- useful? */
		while (i--)
			if (cmds[i][0] == *tmp && !strcmp(cmds[i], tmp))
				break;
		if (i == -1)
			goto closure;
	}
	/* This is where we kill/nice something. */
	/* for debugging purposes?
	fprintf(stderr, "PID %d, UID %d, TTY %d,%d, COMM %s\n",
		pid, statbuf.st_uid, tty >> 8, tty & 0xf, tmp);
	*/
	hurt_proc(tty, statbuf.st_uid, pid, tmp, run_time);
 closure:
	/* kill/nice _first_ to avoid PID reuse */
	close(fd);
}

/* debug function */
static void show_lists(void)
{
	int i;

	fprintf(stderr, _("signal: %d\n"), sig_or_pri);

	fprintf(stderr, _("%d TTY: "), tty_count);
	if (ttys) {
		i = tty_count;
		while (i--) {
			fprintf(stderr, "%d,%d%c", (ttys[i] >> 8) & 0xff,
				ttys[i] & 0xff, i ? ' ' : '\n');
		}
	} else
		fprintf(stderr, "\n");

	fprintf(stderr, _("%d UID: "), uid_count);
	if (uids) {
		i = uid_count;
		while (i--)
			fprintf(stderr, "%d%c", uids[i], i ? ' ' : '\n');
	} else
		fprintf(stderr, "\n");

	fprintf(stderr, _("%d PID: "), pid_count);
	if (pids) {
		i = pid_count;
		while (i--)
			fprintf(stderr, "%d%c", pids[i], i ? ' ' : '\n');
	} else
		fprintf(stderr, "\n");

	fprintf(stderr, _("%d CMD: "), cmd_count);
	if (cmds) {
		i = cmd_count;
		while (i--)
			fprintf(stderr, "%s%c", cmds[i], i ? ' ' : '\n');
	} else
		fprintf(stderr, "\n");
}

/* iterate over all PIDs */
static void iterate(struct run_time_conf_t *run_time)
{
	int pid;
	DIR *d;
	struct dirent *de;
	if (pids) {
		pid = pid_count;
		while (pid--)
			check_proc(pids[pid], run_time);
		return;
	}
#if 0
	/* could setuid() and kill -1 to have the kernel wipe out a user */
	if (!ttys && !cmds && !pids && !run_time->interactive) {
	}
#endif
	d = opendir("/proc");
	if (!d) {
		perror("/proc");
		exit(1);
	}
	while ((de = readdir(d))) {
		if (de->d_name[0] > '9')
			continue;
		if (de->d_name[0] < '1')
			continue;
		pid = atoi(de->d_name);
		if (pid)
			check_proc(pid, run_time);
	}
	closedir(d);
}

/* kill help */
static void __attribute__ ((__noreturn__)) kill_usage(void)
{
	fputs(USAGE_HEADER, stderr);
	fprintf(stderr,
		" %s [options] <pid> [...]\n", program_invocation_short_name);
	fputs(USAGE_OPTIONS, stderr);
	fputs(_("  <pid> [...]    send SIGTERM to every <pid> listed\n"), stderr);
	fputs(_("  -<signal>      specify the <signal> to be sent\n"), stderr);
	fputs(_("  -s <signal>    specify the <signal> to be sent\n"), stderr);
	fputs(_("  -l             list all signal names\n"), stderr);
	fputs(_("  -L             list all signal names in a nice table\n"), stderr);
	fputs(_("  -l <signal>    convert between signal numbers and names\n"), stderr);
	fputs(USAGE_SEPARATOR, stderr);
	fprintf(stderr, USAGE_MAN_TAIL("skill(1)"));
	exit(1);
}

/* skill and snice help */
static void __attribute__ ((__noreturn__)) skillsnice_usage(void)
{
	fputs(USAGE_HEADER, stderr);

	if (program == PROG_SKILL) {
		fprintf(stderr,
			" %s [signal] [options] <expression>\n",
			program_invocation_short_name);
	} else {
		fprintf(stderr,
			" %s [new priority] [options] <expression>\n",
			program_invocation_short_name);
	}
	fputs(USAGE_OPTIONS, stderr);
	fputs(_(" -f             fast mode (not implemented)\n"), stderr);
	fputs(_(" -i             interactive\n"), stderr);
	fputs(_(" -l             list all signal names\n"), stderr);
	fputs(_(" -L             list all signal names in a nice table\n"),
	      stderr);
	fputs(_(" -n             no action\n"), stderr);
	fputs(_(" -v             explain what is being done\n"), stderr);
	fputs(_(" -w             enable warnings (not implemented)\n"), stderr);
	fputs(USAGE_VERSION, stderr);
	fputs(_("\n"), stderr);
	fputs(_("Expression can be: terminal, user, pid, command.\n"), stderr);
	fputs(_("The options below may be used to ensure correct interpretation.\n"), stderr);
	fputs(_(" -c <command>   expression is a command name\n"), stderr);
	fputs(_(" -p <pid>       expression is a process id number\n"), stderr);
	fputs(_(" -t <tty>       expression is a terminal\n"), stderr);
	fputs(_(" -u <username>  expression is a username\n"), stderr);
	if (program == PROG_SKILL) {
		fprintf(stderr,
			_("\n"
			  "The default signal is TERM. Use -l or -L to list available signals.\n"
			  "Particularly useful signals include HUP, INT, KILL, STOP, CONT, and 0.\n"
			  "Alternate signals may be specified in three ways: -SIGKILL -KILL -9\n"));
		fprintf(stderr, USAGE_MAN_TAIL("skill(1)"));
	} else {
		fprintf(stderr,
			_("\n"
			  "The default priority is +4. (snice +4 ...)\n"
			  "Priority numbers range from +20 (slowest) to -20 (fastest).\n"
			  "Negative priority numbers are restricted to administrative users.\n"));
		fprintf(stderr, USAGE_MAN_TAIL("snice(1)"));
	}
	exit(1);
}

/* kill */
static void __attribute__ ((__noreturn__))
kill_main(int argc, char ** argv,
	  struct run_time_conf_t *run_time)
{
	const char *sigptr;
	int signo = SIGTERM;
	int exitvalue = 0;
	if (argc < 2)
		kill_usage();
	if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
		display_kill_version();
		exit(0);
	}
	if (argv[1][0] != '-') {
		argv++;
		argc--;
		goto no_more_args;
	}

	/* The -l option prints out signal names. */
	if (argv[1][1] == 'l' && argv[1][2] == '\0') {
		if (argc == 2) {
			unix_print_signals();
			exit(0);
		}
		/* at this point, argc must be 3 or more */
		if (argc > 128 || argv[2][0] == '-')
			kill_usage();
		exit(print_given_signals(argc - 2, argv + 2, 80));
	}

	/* The -L option prints out signal names in a nice table. */
	if (argv[1][1] == 'L' && argv[1][2] == '\0') {
		if (argc == 2) {
			pretty_print_signals();
			exit(0);
		}
		kill_usage();
	}
	if (argv[1][1] == '-' && argv[1][2] == '\0') {
		argv += 2;
		argc -= 2;
		goto no_more_args;
	}
	if (argv[1][1] == '-')
		kill_usage();	/* likely --help */
	// FIXME: "kill -sWINCH $$" not handled
	if (argv[1][2] == '\0' && (argv[1][1] == 's' || argv[1][1] == 'n')) {
		sigptr = argv[2];
		argv += 3;
		argc -= 3;
	} else {
		sigptr = argv[1] + 1;
		argv += 2;
		argc -= 2;
	}
	signo = signal_name_to_number(sigptr);
	if (signo < 0) {
		fprintf(stderr, _("ERROR: unknown signal name \"%s\".\n"),
			sigptr);
		kill_usage();
	}
 no_more_args:
	if (!argc)
		kill_usage();	/* nothing to kill? */
	while (argc--) {
		long pid;
		char *endp;
		pid = strtol(argv[argc], &endp, 10);
		if (!*endp && (endp != argv[argc])) {
			if (!kill((pid_t) pid, signo))
				continue;
			/*
			 * The UNIX standard contradicts itself. If at least
			 * one process is matched for each PID (as if
			 * processes could share PID!) and "the specified
			 * signal was successfully processed" (the systcall
			 * returned zero?) for at least one of those
			 * processes, then we must exit with zero.  Note that
			 * an error might have also occured.  The standard
			 * says we return non-zero if an error occurs.  Thus
			 * if killing two processes gives 0 for one and EPERM
			 * for the other, we are required to return both zero
			 * and non-zero.  Quantum kill???
			 */
			perror("kill");
			exitvalue = 1;
			continue;
		}
		fprintf(stderr, _("ERROR: garbage process ID \"%s\".\n"),
			argv[argc]);
		kill_usage();
	}
	exit(exitvalue);
}
#if 0
static void _skillsnice_usage(int line)
{
	fprintf(stderr, _("Something at line %d.\n"), line);
	skillsnice_usage();
}

#define skillsnice_usage() _skillsnice_usage(__LINE__)
#endif

#define NEXTARG (argc?( argc--, ((argptr=*++argv)) ):NULL)

/* common skill/snice argument parsing code */

int snice_prio_option(int *argc, char **argv)
{
	int i = 1, nargs = *argc;
	long prio = DEFAULT_NICE;

	while (i < nargs) {
		if ((argv[i][0] == '-' || argv[i][0] == '+')
		    && isdigit(argv[i][1])) {
			prio = strtol_or_err(argv[i],
					     _("failed to parse argument"));
			if (prio < INT_MIN || INT_MAX < prio)
				errx(EXIT_FAILURE,
				     _("priority %lu out of range"), prio);
			nargs--;
			if (nargs - i)
				memmove(argv + i, argv + i + 1,
					sizeof(char *) * (nargs - i));
		} else
			i++;
	}
	*argc = nargs;
	return (int)prio;
}

int skill_sig_option(int *argc, char **argv)
{
	int i, nargs = *argc;
	int signo = -1;
	for (i = 1; i < nargs; i++) {
		if (argv[i][0] == '-') {
			signo = signal_name_to_number(argv[i] + 1);
			if (-1 < signo) {
				if (nargs - i) {
					nargs--;
					memmove(argv + i, argv + i + 1,
						sizeof(char *) * (nargs - i));
				}
				return signo;
			}
		}
	}
	return signo;
}

static void skillsnice_parse(int argc,
			     char **argv, struct run_time_conf_t *run_time)
{
	int signo = -1;
	int prino = DEFAULT_NICE;
	int force = 0;
	int num_found = 0;
	int ch, i;
	const char *restrict argptr;

	static const struct option longopts[] = {
		{"command", required_argument, NULL, 'c'},
		{"debug", no_argument, NULL, 'd'},
		{"fast", no_argument, NULL, 'f'},
		{"interactive", no_argument, NULL, 'i'},
		{"list", no_argument, NULL, 'l'},
		{"no-action", no_argument, NULL, 'n'},
		{"pid", required_argument, NULL, 'p'},
		{"table", no_argument, NULL, 'L'},
		{"tty", required_argument, NULL, 't'},
		{"user", required_argument, NULL, 'u'},
		{"verbose", no_argument, NULL, 'v'},
		{"warnings", no_argument, NULL, 'w'},
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};

	if (argc < 2)
		skillsnice_usage();

	sig_or_pri = -1;

	if (program == PROG_SNICE)
		prino = snice_prio_option(&argc, argv);
	else if (program == PROG_SKILL) {
		signo = skill_sig_option(&argc, argv);
		if (-1 < signo)
			sig_or_pri = signo;
	}

	pid_count = 0;

	while ((ch =
		getopt_long(argc, argv, "c:dfilnp:Lt:u:vwhV", longopts,
			    NULL)) != -1)
		switch (ch) {
		case 'c':
			ENLIST(cmd, optarg);
			break;
		case 'd':
			run_time->debugging = 1;
			break;
		case 'f':
			run_time->fast = 1;
			break;
		case 'i':
			run_time->interactive = 1;
			break;
		case 'l':
			unix_print_signals();
			exit(EXIT_SUCCESS);
		case 'n':
			run_time->noaction = 1;
			break;
		case 'p':
			ENLIST(pid,
			       strtol_or_err(optarg,
					     _("failed to parse argument")));
			pid_count++;
			break;
		case 'L':
			pretty_print_signals();
			exit(EXIT_SUCCESS);
		case 't':
			{
				struct stat sbuf;
				char path[32];
				if (!optarg)
					/* Huh? Maybe "skill -t ''". */
					skillsnice_usage();
				snprintf(path, 32, "/dev/%s", optarg);
				if (stat(path, &sbuf) >= 0
				    && S_ISCHR(sbuf.st_mode)) {
					num_found++;
					ENLIST(tty, sbuf.st_rdev);
					if (!NEXTARG)
						break;
				} else if (!(optarg[1])) {
					/* if only 1 character */
					switch (*optarg) {
					default:
						if (stat(optarg, &sbuf) < 0)
							/* the shell eats '?' */
							break;
					case '-':
					case '?':
						num_found++;
						ENLIST(tty, 0);
						if (!NEXTARG)
							break;
					}
				}
			}
			break;
		case 'u':
			{
				struct passwd *passwd_data;
				passwd_data = getpwnam(argptr);
				if (passwd_data) {
					num_found++;
					ENLIST(uid, passwd_data->pw_uid);
					if (!NEXTARG)
						break;
				}
			}
			break;
		case 'v':
			run_time->verbose = 1;
			break;
		case 'w':
			run_time->warnings = 1;
			break;
		case 'h':
			skillsnice_usage();
		case 'V':
			display_kill_version();
			exit(EXIT_SUCCESS);
		default:
			skillsnice_usage();
		}

	argc -= optind;
	argv += optind;

	for (i = 0; i < argc; i++) {
		ENLIST(pid, strtol_or_err(argv[0],
					  _("failed to parse argument")));
		pid_count++;
		argv++;
	}

	/* No more arguments to process. Must sanity check. */
	if (!tty_count && !uid_count && !cmd_count && !pid_count)
		errx(EXIT_FAILURE, _("no process selection criteria"));
	if ((run_time->fast | run_time->interactive | run_time->
	     verbose | run_time->warnings | run_time->noaction) & ~1)
		errx(EXIT_FAILURE, _("general flags may not be repeated"));
	if (run_time->interactive
	    && (run_time->verbose | run_time->fast | run_time->noaction))
		errx(EXIT_FAILURE, _("-i makes no sense with -v, -f, and -n"));
	if (run_time->verbose && (run_time->interactive | run_time->fast))
		errx(EXIT_FAILURE, _("-v makes no sense with -i and -f"));
	if (run_time->noaction) {
		program = PROG_SKILL;
		/* harmless */
		sig_or_pri = 0;
	}
	if (program == PROG_SNICE)
		sig_or_pri = prino;
	else if (sig_or_pri < 0)
		sig_or_pri = SIGTERM;
}

/* main body */
int main(int argc, char ** argv)
{
	struct run_time_conf_t run_time;
	memset(&run_time, 0, sizeof(struct run_time_conf_t));
	my_pid = getpid();

	if (strcmp(program_invocation_short_name, "kill") == 0 ||
	    strcmp(program_invocation_short_name, "lt-kill") == 0)
		program = PROG_KILL;
	else if (strcmp(program_invocation_short_name, "skill") == 0 ||
		 strcmp(program_invocation_short_name, "lt-skill") == 0)
		program = PROG_SKILL;
	else if (strcmp(program_invocation_short_name, "snice") == 0 ||
		 strcmp(program_invocation_short_name, "lt-snice") == 0)
		program = PROG_SNICE;

	switch (program) {
	case PROG_SNICE:
	case PROG_SKILL:
		setpriority(PRIO_PROCESS, my_pid, -20);
		skillsnice_parse(argc, argv, &run_time);
		if (run_time.debugging)
			show_lists();
		iterate(&run_time);
		break;
	case PROG_KILL:
		kill_main(argc, argv, &run_time);
		break;
	default:
		fprintf(stderr, _("skill: \"%s\" is not support\n"),
			program_invocation_short_name);
		fprintf(stderr, USAGE_MAN_TAIL("skill(1)"));
		return 1;
	}
	return 0;
}
