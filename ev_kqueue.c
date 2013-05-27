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

#include "config.h"
#include "dircond.h"
#include <sys/event.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

struct event events[] = {
	{ NOTE_DELETE, "delete" },
	{ NOTE_WRITE,  "write" },
	{ NOTE_EXTEND, "extend" },
	{ NOTE_ATTRIB, "attrib" },
	{ NOTE_LINK,   "link" },
	{ NOTE_RENAME, "rename" },
	{ NOTE_REVOKE, "revoke" },
	{ 0, NULL }
};

int
evsys_name_to_code(const char *name)
{
	int i;

	for (i = 0; events[i].evname; i++) {
		if (strcmp(events[i].evname, name) == 0)
			return events[i].evcode;
	}
	return 0;
}

const char *
evsys_code_to_name(int code)
{
	int i;

	for (i = 0; events[i].evname; i++) {
		if (events[i].evcode & code)
			return events[i].evname;
	}
	return NULL;
}

static void
ev_log(struct kevent *ep, struct dirwatcher *dp)
{
	int i;

	if (debug_level > 0) {
		for (i = 0; events[i].evname; i++) {
			if (events[i].evcode & ep->fflags)
				debug(1, ("%s: %s", dp->dirname,
					  events[i].evname));
		}
	}
}
		

static int kq;
static struct kevent *evtab;
static struct kevent *chtab;
static int chcnt;
static int chclosed = -1;

int evsys_filemask = S_IFMT;

void
evsys_init()
{
	kq = kqueue();
	if (kq == -1) {
		diag(LOG_CRIT, "kqueue: %s", strerror(errno));
		exit(1);
	}
	evtab = calloc(sysconf(_SC_OPEN_MAX), sizeof(evtab[0]));
	chtab = calloc(sysconf(_SC_OPEN_MAX), sizeof(chtab[0]));
}

int
evsys_add_watch(struct dirwatcher *dwp, int mask)
{
	int wd = open(dwp->dirname, O_RDONLY);
	if (wd >= 0) {
		EV_SET(chtab + chcnt, wd, EVFILT_VNODE,
		       EV_ADD | EV_ENABLE | EV_ONESHOT, mask,
		       0, dwp);
		wd = chcnt++;
	}
	return wd;
}

void
evsys_rm_watch(struct dirwatcher *dwp)
{
	close(chtab[dwp->wd].ident);
	chtab[dwp->wd].ident = -1;
	if (chclosed != -1 && chclosed > dwp->wd)
		chclosed = dwp->wd;
}

static void
chclosed_elim()
{
	int i, j;
	
	if (chclosed == -1)
		return;

	for (i = chclosed, j = chclosed + 1; j < chcnt; j++)
		if (chtab[j].ident != -1) {
			struct dirwatcher *dwp;
			
			chtab[i] = chtab[j];
			dwp = chtab[i].udata;
			dwp->wd = i;
			i++;
		}
	chcnt = i;
	chclosed = -1;
}

static void
process_event(struct kevent *ep)
{
	struct dirwatcher *dp = ep->udata;
	struct handler *h;

	if (!dp) {
		diag(LOG_NOTICE, "unrecognized event %x", ep->fflags);
		return;
	}

	ev_log(ep, dp);

	for (h = dp->handler_list; h; h = h->next) {
		if (h->ev_mask & ep->fflags)
			run_handler(dp->parent, h, ep->fflags, dp->dirname);
	}
}	


void
evsys_loop()
{
	int i, n;
	
	/* Main loop */
	while (1) {
		chclosed_elim();
		n = kevent(kq, chtab, chcnt, evtab, chcnt, NULL);
		if (n == -1) {
			if (signo == SIGCHLD || signo == SIGALRM)
				continue;
			diag(LOG_NOTICE, "got signal %d", signo);
			break;
		} 

		for (i = 0; i < n; i++) {
			process_event(&evtab[i]);
		}
	}
}
		
