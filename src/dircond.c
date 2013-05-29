/* dircond - directory content watcher daemon
   Copyright (C) 2012, 2013 Sergey Poznyakoff

   Dircond is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3 of the License, or (at your
   option) any later version.

   Dircond is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with dircond. If not, see <http://www.gnu.org/licenses/>. */

#include "dircond.h"
#include <stdarg.h>
#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>

#ifndef SYSCONFDIR
# define SYSCONFDIR "/etc"
#endif
#define DEFAULT_CONFFILE SYSCONFDIR "/dircond.conf"

/* Configuration settings */
const char *program_name;         /* This program name */
const char *conffile = DEFAULT_CONFFILE;
int foreground;                   /* Remain in the foreground */
char *tag;                        /* Syslog tag */
int facility = -1;                /* Use this syslog facility for logging.
				     -1 means log to stderr */
int debug_level;                  /* Debug verbosity level */
char *pidfile = NULL;             /* Store PID to this file */
char *user = NULL;                /* User to run as */


/* Diagnostic functions */
const char *
severity(int prio)
{
	switch (prio) {
	case LOG_EMERG:
		return "EMERG";
	case LOG_ALERT:
		return "ALERT";
	case LOG_CRIT:
		return "CRIT";
	case LOG_ERR:
		return "ERROR";
	case LOG_WARNING:
		return "WARNING";
	case LOG_NOTICE:
		return "NOTICE";
	case LOG_INFO:
		return "INFO";
	case LOG_DEBUG:
		return "DEBUG";
	}
	return NULL;
}

void
vdiag(int prio, const char *fmt, va_list ap)
{
	const char *s;
	
	if (facility <= 0) {
		fprintf(stderr, "%s: ", program_name);
		s = severity(prio);
		if (s)
			fprintf(stderr, "[%s] ", s);
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else {
		vsyslog(prio, fmt, ap);
	}
}

void
diag(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vdiag(prio, fmt, ap);
	va_end(ap);
}
	
void
debugprt(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vdiag(LOG_DEBUG, fmt, ap);
	va_end(ap);
}

/* Memory allocation with error checking */
void *
emalloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		diag(LOG_CRIT, "not enough memory");
		exit(2);
	}
	return p;
}

void *
ecalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);
	if (!p) {
		diag(LOG_CRIT, "not enough memory");
		exit(2);
	}
	return p;
}

void *
erealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);
	if (!p) {
		diag(LOG_CRIT, "not enough memory");
		exit(2);
	}
	return p;
}

char *
estrdup(const char *str)
{
	size_t len = strlen(str);
	char *p = emalloc(len + 1);
	memcpy(p, str, len);
	p[len] = 0;
	return p;
}

/* Create a full file name from directory and file name */
char *
mkfilename(const char *dir, const char *file)
{
	char *tmp;
	size_t dirlen = strlen(dir);
	size_t fillen = strlen(file);
	size_t len;

	while (dirlen > 0 && dir[dirlen-1] == '/')
		dirlen--;

	len = dirlen + (dir[0] ? 1 : 0) + fillen;
	tmp = malloc(len + 1);
	if (tmp) {
		memcpy(tmp, dir, dirlen);
		if (dir[0])
			tmp[dirlen++] = '/';
		memcpy(tmp + dirlen, file, fillen);
		tmp[len] = 0;
	}
	return tmp;
}

int
trans_strtotok(struct transtab *tab, const char *str, int *ret)
{
	for (; tab->name; tab++)
		if (strcmp(tab->name, str) == 0) {
			*ret = tab->tok;
			return 0;
		}
	return -1;
}

char *
trans_toktostr(struct transtab *tab, int tok)
{
	for (; tab->name; tab++)
		if (tab->tok == tok)
			return tab->name;
	return NULL;
}

char *
trans_toknext(struct transtab *tab, int tok, int *next)
{
	int i;
	
	for (i = *next; tab[i].name; i++)
		if (tab[i].tok & tok) {
			*next = i + 1;
			return tab[i].name;
		}
	*next = i;
	return NULL;
}

char *
trans_tokfirst(struct transtab *tab, int tok, int *next)
{
	*next = 0;
	return trans_toknext(tab, tok, next);
}


/* Command line processing and auxiliary functions */

static void
set_program_name(const char *arg)
{
	char *p = strrchr(arg, '/');
	if (p)
		program_name = p + 1;
	else
		program_name = arg;
}


void
signal_setup(void (*sf) (int))
{
	signal(SIGTERM, sf);
	signal(SIGQUIT, sf);
	signal(SIGINT, sf);
	signal(SIGHUP, sf);
	signal(SIGALRM, sf);
	signal(SIGUSR1, sf);
	signal(SIGUSR2, sf);
}

void
storepid(const char *pidfile)
{
	FILE *fp = fopen(pidfile, "w");
	if (!fp) {
		diag(LOG_ERR, "cannot open pidfile %s for writing: %s",
		     pidfile, strerror(errno));
	} else {
		fprintf(fp, "%lu\n", (unsigned long) getpid());
		fclose(fp);
	}
}

static int
membergid(gid_t gid, size_t gc, gid_t *gv)
{
	int i;
	for (i = 0; i < gc; i++)
		if (gv[i] == gid)
			return 1;
	return 0;
}

static void
get_user_groups(uid_t uid, size_t *pgidc, gid_t **pgidv)
{
	size_t gidc = 0, n = 0;
	gid_t *gidv = NULL;
	struct passwd *pw;
	struct group *gr;

	pw = getpwuid(uid);
	if (!pw) {
		diag(LOG_ERR, 0, "no used with UID %lu",
		     (unsigned long)uid);
		exit(2);
	}
	
	n = 32;
	gidv = ecalloc(n, sizeof(gidv[0]));
		
	gidv[0] = pw->pw_gid;
	gidc = 1;
	
	setgrent();
	while (gr = getgrent()) {
		char **p;
		for (p = gr->gr_mem; *p; p++)
			if (strcmp(*p, pw->pw_name) == 0) {
				if (n == gidc) {
					n += 32;
					gidv = erealloc(gidv,
							n * sizeof(gidv[0]));
				}
				if (!membergid(gr->gr_gid, gidc, gidv))
					gidv[gidc++] = gr->gr_gid;
			}
	}
	endgrent();
	*pgidc = gidc;
	*pgidv = gidv;
}

void
setuser(const char *user)
{
	struct passwd *pw;
	size_t gidc;
	gid_t *gidv;
		
	pw = getpwnam(user);
	if (!pw) {
		diag(LOG_CRIT, "getpwnam(%s): %s", user, strerror(errno));
		exit(2);
	}
	if (pw->pw_uid == 0)
		return;

	get_user_groups(pw->pw_uid, &gidc, &gidv);
	if (setgroups(gidc, gidv) < 0) {
		diag(LOG_CRIT, "setgroups: %s", strerror(errno));
		exit(2);
	}
	free(gidv);

	if (setgid(pw->pw_gid)) {
		diag(LOG_CRIT, "setgid(%lu): %s", (unsigned long) pw->pw_gid,
		     strerror(errno));
		exit(2);
	}
	if (setuid(pw->pw_uid)) {
		diag(LOG_CRIT, "setuid(%lu): %s", (unsigned long) pw->pw_uid,
		     strerror(errno));
		exit(2);
	}
}

void
ev_log(int flags, struct dirwatcher *dp)
{
	int i;
	char *p;
	
	if (debug_level > 0) {
		for (p = trans_tokfirst(evsys_transtab, flags, &i); p;
		     p = trans_toknext(evsys_transtab, flags, &i))
			debug(1, ("%s: %s", dp->dirname, p));
	}
}


#if USE_IFACE == IFACE_INOTIFY
# define INTERFACE "inotify"
#elif USE_IFACE == IFACE_KQUEUE
# define INTERFACE "kqueue"
#endif

/* Output a help summary. Return a code suitable for exit(2). */
int
help()
{
	printf("Usage: %s [OPTIONS] [CONFIG]\n", program_name);
	printf("%s monitors changes in directories\n", program_name);
	printf("OPTIONS are:\n\n");

	printf("   -d            increase debug verbosity\n");
	printf("   -F FACILITY   log under this syslog facility (default: daemon);\n");
	printf("                 use -F 0 to log to stderr instead\n");
        printf("   -f            run in the foreground\n");
        printf("   -L TAG        log with this syslog tag\n");
	printf("   -P FILE       write PID to FILE\n");
	printf("   -t            check configuration file for errors and exit\n");
	printf("   -u USER       run as this USER\n\n");

	printf("   -h            output this help summary\n");
        printf("   -V            print program version and exit\n\n");

	printf("Optional CONFIG argument supplies the configuration file\n"
	       "to use instead of %s.\n\n", DEFAULT_CONFFILE); 

	printf("This dircond uses %s interface.\n\n", INTERFACE);
	
	printf("Report bugs to <%s>.\n", PACKAGE_BUGREPORT);
		
	return 0;
}

static char license[] = "\
License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n\
This is free software: you are free to change and redistribute it.\n\
There is NO WARRANTY, to the extent permitted by law.\n";

int
version()
{
	printf("dircond %s\n", VERSION);
	printf("Copyright (C) 2012, 2013 Sergey Poznyakoff\n");
	printf("%s\n", license);
	return 0;
}

int
get_facility(const char *arg)
{
	int f;
	
	if (read_facility(arg, &f)) {
		switch (errno) {
		case EINVAL:
			diag(LOG_CRIT,
			     "unknown syslog facility: %s", arg);
			break;

		case ERANGE:
			diag(LOG_CRIT, "syslog facility out of range");
			break;
				
		default:
			abort();
		}
		exit(1);
	}
	return f;
}


void
sie_init()
{
	int i;
	
	for (i = 0; i < sie_xlat[i].sie_mask; i++)
		defevt(trans_toktostr(sie_trans, sie_xlat[i].sie_mask),
		       &sie_xlat[i], 0);
}
	

int signo = 0;

void
sigmain(int sig)
{
	signo = sig;
	signal(sig, sigmain);
}

int
main(int argc, char **argv)
{
	int c;
	int opt_debug_level = 0;
	int opt_foreground = 0;
	char *opt_tag = NULL;
	char *opt_pidfile = NULL;
	char *opt_user = NULL;
	int lint_only = 0;
	
	set_program_name(argv[0]);
	tag = (char*) program_name;

	evsys_init();
	sie_init();
	
	while ((c = getopt(argc, argv, "dF:fhLP:tu:V")) != EOF) {
		switch (c) {
		case 'd':
			opt_debug_level++;
			break;
		case 'F':			
			opt_facility = get_facility(optarg);
			break;
		case 'f':
			opt_foreground++;
			break;
		case 'h':
			exit(help());
			break;
		case 'L':
			opt_tag = optarg;
			break;
		case 'P':
			opt_pidfile = optarg;
			break;
		case 't':
			lint_only = 1;
			break;
		case 'u':
			opt_user = optarg;
			if (!getpwnam(opt_user)) {
				diag(LOG_CRIT, "no such user: %s", opt_user);
				exit(1);
			}
			break;
		case 'V':
			exit(version());
		default:
			exit(1);
		}
	}

	argc -= optind;
	argv += optind;

	switch (argc) {
	default:
		diag(LOG_CRIT, "too many arguments");
		exit(1);
	case 1:
		conffile = argv[0];
		break;
	case 0:
		break;
	}

	config_parse(conffile);
	if (lint_only)
		return 0;
	
	if (opt_debug_level)
		debug_level += opt_debug_level;
	if (opt_foreground)
		foreground = opt_foreground;
	if (opt_tag)
		tag = opt_tag;
	if (opt_pidfile)
		pidfile = opt_pidfile;
	if (opt_facility != -1)
		facility = opt_facility;
	if (opt_user)
		user = opt_user;
	
	setup_watchers();

	/* Become a daemon */
	if (!foreground) {
		if (daemon(0, 0)) {
			diag(LOG_CRIT, "daemon: %s", strerror(errno));
			exit(1);
		}
		if (facility <= 0)
			facility = LOG_DAEMON;
	}
	
	if (facility > 0)
		openlog(tag, LOG_PID, facility);

	diag(LOG_INFO, "started");

	/* Write pidfile */
	if (pidfile)
		storepid(pidfile);

	/* Relinquish superuser privileges */
	if (user && getuid() == 0)
		setuser(user);

	signal_setup(sigmain);
	signal(SIGCHLD, sigmain);

	/* Main loop */
	do {
		process_timeouts();
		process_cleanup(0);
	} while (evsys_select () == 0);

	diag(LOG_INFO, "stopped");

	return 0;
}