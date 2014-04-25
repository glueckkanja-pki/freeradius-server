/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License, version 2 if the
 *   License as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_unbound.c
 * @brief DNS services via libunbound.
 *
 * @copyright 2013 The FreeRADIUS server project
 * @copyright 2013 Brian S. Julin <bjulin@clarku.edu>
 */
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/log.h>
#include <fcntl.h>
#include <unbound.h>

typedef struct rlm_unbound_t {
	struct ub_ctx	*ub;   /* This must come first.  Do not move */
	fr_event_list_t	*el; /* This must come second.  Do not move. */

	char const	*name;
	char const	*xlat_a_name;
	char const	*xlat_aaaa_name;
	char const	*xlat_ptr_name;

	char		*filename;
	int		timeout;
	int		fd, logfd[2];
	FILE		*logstream[2];
	int		pipe_inuse;

	FILE		*debug_stream;
} rlm_unbound_t;

/*
 *	A mapping of configuration file names to internal variables.
 */
static const CONF_PARSER module_config[] = {
	{ "filename", PW_TYPE_FILE_INPUT | PW_TYPE_REQUIRED, offsetof(rlm_unbound_t, filename), NULL,
	  "${modconfdir}/unbound/default.conf" },
	{ "timeout", PW_TYPE_INTEGER, offsetof(rlm_unbound_t, timeout), NULL, "3000" },
	{ NULL, -1, 0, NULL, NULL }		/* end the list */
};

/*
 *	Callback sent to libunbound for xlat functions.  Simply links the
 *	new ub_result via a pointer that has been allocated from the heap.
 *	This pointer has been pre-initialized to a magic value.
 */
static void link_ubres(void* my_arg, int err, struct ub_result* result)
{
	struct ub_result **ubres = (struct ub_result **)my_arg;

	/*
	 *	Note that while result will be NULL on error, we are explicit
	 *	here because that is actually a behavior that is suboptimal
	 *	and only documented in the examples.  It could change.
	 */
	if (err) {
		EDEBUG("rlm_unbound: %s", ub_strerror(err));
		*ubres = NULL;
	} else {
		*ubres = result;
	}
}

/*
 *	Convert labels as found in a DNS result to a NULL terminated string.
 *
 *	Result is written to memory pointed to by "out" but no result will
 *	be written unless it and its terminating NULL character fit in "left"
 *	bytes.  Returns the number of bytes written excluding the terminating
 *	NULL, or -1 if nothing was written because it would not fit or due
 *	to a violation in the labels format.
 */
static int rrlabels_tostr(char *out, char *rr, size_t left)
{
	int offset = 0;

	/*
	 * TODO: verify that unbound results (will) always use this label
	 * format, and review the specs on this label format for nuances.
	 */

	if (!left) {
		return -1;
	}
	if (left > 253) {
		left = 253; /* DNS length limit */
	}
	/* As a whole this should be "NULL terminated" by the 0-length label */
	if (strnlen(rr, left) > left - 1) {
		return -1;
	}

	/* It will fit, but does it it look well formed? */
	while (1) {
		size_t count;

		count = *((unsigned char *)(rr + offset));
		if (!count) break;

		offset++;
		if (count > 63 || strlen(rr + offset) < count) {
			return -1;
		}
		offset += count;
	}

	/* Data is valid and fits.  Copy it. */
	offset = 0;
	while (1) {
		int count;

		count = *((unsigned char *)(rr));
		if (!count) break;

		if (offset) {
			*(out + offset) = '.';
			offset++;
		}

		rr++;
		memcpy(out + offset, rr, count);
		rr += count;
		offset += count;
	}

	*(out + offset) = '\0';
	return offset;
}

static int ub_common_wait(rlm_unbound_t *inst, REQUEST *request, char const *tag, struct ub_result **ub, int async_id)
{
	useconds_t iv, waited;

	iv = inst->timeout > 64 ? 64000 : inst->timeout * 1000;
	ub_process(inst->ub);

	for (waited = 0; (void*)*ub == (void *)inst; waited += iv, iv += iv) {

		if (waited + iv > (useconds_t)inst->timeout * 1000) {
			usleep(inst->timeout * 1000 - waited);
			ub_process(inst->ub);
			break;
		}

		usleep(iv);

		/* Check if already handled by event loop */
		if ((void *)*ub != (void *)inst) {
			break;
		}

		/* In case we are running single threaded */
		ub_process(inst->ub);
	}

	if ((void *)*ub == (void *)inst) {
		int res;

		RDEBUG("rlm_unbound (%s): DNS took too long", tag);

		res = ub_cancel(inst->ub, async_id);
		if (res) {
			REDEBUG("rlm_unbound (%s): ub_cancel: %s",
				tag, ub_strerror(res));
		}
		return -1;
	}

	return 0;
}

static int ub_common_fail(REQUEST *request, char const *tag, struct ub_result *ub)
{
	if (ub->bogus) {
		RWDEBUG("rlm_unbound (%s): Bogus DNS response", tag);
		return -1;
	}

	if (ub->nxdomain) {
		RDEBUG("rlm_unbound (%s): NXDOMAIN", tag);
		return -1;
	}

	if (!ub->havedata) {
		RDEBUG("rlm_unbound (%s): empty result", tag);
		return -1;
	}

	return 0;
}

static ssize_t xlat_a(void *instance, REQUEST *request, char const *fmt, char *out, size_t freespace)
{
	rlm_unbound_t *inst = instance;
	struct ub_result **ubres;
	int async_id;
	char *fmt2; /* For const warnings.  Keep till new libunbound ships. */

	/* This has to be on the heap, because threads. */
	ubres = talloc(inst, struct ub_result *);

	/* Used and thus impossible value from heap to designate incomplete */
	*ubres = (void *)instance;

	fmt2 = talloc_typed_strdup(inst, fmt);
	ub_resolve_async(inst->ub, fmt2, 1, 1, ubres, link_ubres, &async_id);
	talloc_free(fmt2);

	if (ub_common_wait(inst, request, inst->xlat_a_name, ubres, async_id)) {
		goto error0;
	}

	if (*ubres) {
		if (ub_common_fail(request, inst->xlat_a_name, *ubres)) {
			goto error1;
		}

		if (!inet_ntop(AF_INET, (*ubres)->data[0], out, freespace)) {
			goto error1;
		};

		ub_resolve_free(*ubres);
		talloc_free(ubres);
		return strlen(out);
	}

	RWDEBUG("rlm_unbound (%s): no result", inst->xlat_a_name);

 error1:
	ub_resolve_free(*ubres); /* Handles NULL gracefully */

 error0:
	talloc_free(ubres);
	return -1;
}

static ssize_t xlat_aaaa(void *instance, REQUEST *request, char const *fmt, char *out, size_t freespace)
{
	rlm_unbound_t *inst = instance;
	struct ub_result **ubres;
	int async_id;
	char *fmt2; /* For const warnings.  Keep till new libunbound ships. */

	/* This has to be on the heap, because threads. */
	ubres = talloc(inst, struct ub_result *);

	/* Used and thus impossible value from heap to designate incomplete */
	*ubres = (void *)instance;

	fmt2 = talloc_typed_strdup(inst, fmt);
	ub_resolve_async(inst->ub, fmt2, 28, 1, ubres, link_ubres, &async_id);
	talloc_free(fmt2);

	if (ub_common_wait(inst, request, inst->xlat_aaaa_name, ubres, async_id)) {
		goto error0;
	}

	if (*ubres) {
		if (ub_common_fail(request, inst->xlat_aaaa_name, *ubres)) {
			goto error1;
		}
		if (!inet_ntop(AF_INET6, (*ubres)->data[0], out, freespace)) {
			goto error1;
		};
		ub_resolve_free(*ubres);
		talloc_free(ubres);
		return strlen(out);
	}

	RWDEBUG("rlm_unbound (%s): no result", inst->xlat_aaaa_name);

error1:
	ub_resolve_free(*ubres); /* Handles NULL gracefully */

error0:
	talloc_free(ubres);
	return -1;
}

static ssize_t xlat_ptr(void *instance, REQUEST *request, char const *fmt, char *out, size_t freespace)
{
	rlm_unbound_t *inst = instance;
	struct ub_result **ubres;
	int async_id;
	char *fmt2; /* For const warnings.  Keep till new libunbound ships. */

	/* This has to be on the heap, because threads. */
	ubres = talloc(inst, struct ub_result *);

	/* Used and thus impossible value from heap to designate incomplete */
	*ubres = (void *)instance;

	fmt2 = talloc_typed_strdup(inst, fmt);
	ub_resolve_async(inst->ub, fmt2, 12, 1, ubres, link_ubres, &async_id);
	talloc_free(fmt2);

	if (ub_common_wait(inst, request, inst->xlat_ptr_name,
			   ubres, async_id)) {
		goto error0;
	}

	if (*ubres) {
		if (ub_common_fail(request, inst->xlat_ptr_name, *ubres)) {
			goto error1;
		}
		if (rrlabels_tostr(out, (*ubres)->data[0], freespace) < 0) {
			goto error1;
		}
		ub_resolve_free(*ubres);
		talloc_free(ubres);
		return strlen(out);
	}

	RWDEBUG("rlm_unbound (%s): no result", inst->xlat_ptr_name);

error1:
	ub_resolve_free(*ubres);  /* Handles NULL gracefully */

error0:
	talloc_free(ubres);
	return -1;
}

/*
 *	Even when run in asyncronous mode, callbacks sent to libunbound still
 *	must be run in an application-side thread (via ub_process.)  This is
 *	probably to keep the API usage consistent across threaded and forked
 *	embedded client modes.  This callback function lets an event loop call
 *	ub_process when the instance's file descriptor becomes ready.
 */
static void ub_fd_handler(UNUSED fr_event_list_t *el, UNUSED int sock, void *ctx)
{
	rlm_unbound_t *inst = ctx;
	int err;

	err = ub_process(inst->ub);
	if (err) {
		ERROR("rlm_unbound (%s) async ub_process: %s",
		      inst->name, ub_strerror(err));
	}
}

#ifndef HAVE_PTHREAD_H

/* If we have to use a pipe to redirect logging, this does the work. */
static void log_spew(UNUSED fr_event_list_t *el, UNUSED int sock, void *ctx)
{
	rlm_unbound_t *inst = ctx;
	char line[1024];

	/*
	 *  This works for pipes from processes, but not from threads
	 *  right now.  The latter is hinky and will require some fancy
	 *  blocking/nonblocking trickery which is not figured out yet,
	 *  since selecting on a pipe from a thread in the same process
	 *  seems to behave differently.  It will likely preclude the use
	 *  of fgets and streams.  Left for now since some unbound logging
	 *  infrastructure is still global across multiple contexts.  Maybe
	 *  we can get unbound folks to provide a ub_ctx_debugout_async that
	 *  takes a function hook instead to just bypass the piping when
	 *  used in threaded mode.
	 */
	while (fgets(line, 1024, inst->logstream[0])) {
		DEBUG("rlm_unbound (%s): %s", inst->name, line);
	}
}

#endif

static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	rlm_unbound_t *inst = instance;
	int res, dlevel;
	int debug_method;
	char *optval;
	int debug_fd = -1;
	char k[64]; /* To silence const warns until newer unbound in distros */

	inst->el = radius_event_list_corral(EVENT_CORRAL_AUX);
	inst->logstream[0] = NULL;
	inst->logstream[1] = NULL;
	inst->fd = -1;
	inst->pipe_inuse = 0;

	inst->name = cf_section_name2(conf);
	if (!inst->name) {
		inst->name = cf_section_name1(conf);
	}

	if ((inst->timeout < 0) || (inst->timeout > 10000)) {
		ERROR("rlm_unbound (%s): timeout must be 0 to 10000", inst->name);
		return -1;
	}

	inst->ub = ub_ctx_create();
	if (!inst->ub) {
		ERROR("rlm_unbound (%s): ub_ctx_create failed", inst->name);
		return -1;
	}

#ifdef HAVE_PTHREAD_H
	/*
	 *	Note unbound threads WILL happen with -s option, if it matters.
	 *	We cannot tell from here whether that option is in effect.
	 */
	res = ub_ctx_async(inst->ub, 1);
#else
	/*
	 *	Uses forked subprocesses instead.
	 */
	res = ub_ctx_async(inst->ub, 0);
#endif

	if (res) goto error;

	/*	Glean some default settings to match the main server.	*/
	/*	TODO: debug_level can be changed at runtime. */
	/*	TODO: log until fork when stdout or stderr and !debug_flag. */
	dlevel = 0;

	if (debug_flag > 0) {
		dlevel = debug_flag;

	} else if (mainconfig.debug_level > 0) {
		dlevel = mainconfig.debug_level;
	}

	switch (dlevel) {
	/* TODO: This will need some tweaking */
	case 0:
	case 1:
		break;

	case 2:
		dlevel = 1;
		break;

	case 3:
	case 4:
		dlevel = 2; /* mid-to-heavy levels of output */
		break;

	case 5:
	case 6:
	case 7:
	case 8:
		dlevel = 3; /* Pretty crazy amounts of output */
		break;

	default:
		dlevel = 4; /* Insane amounts of output including crypts */
		break;
	}

	res = ub_ctx_debuglevel(inst->ub, dlevel);
	if (res) goto error;

	switch(default_log.dst) {
	case L_DST_STDOUT:
		if (!debug_flag) {
			debug_method = 3;
			break;
		}
		debug_method = 1;
		debug_fd = dup(STDOUT_FILENO);
		break;

	case L_DST_STDERR:
		if (!debug_flag) {
			debug_method = 3;
			break;
		}
		debug_method = 1;
		debug_fd = dup(STDERR_FILENO);
		break;

	case L_DST_FILES:
		if (mainconfig.log_file) {
			strcpy(k, "logfile:");
			res = ub_ctx_set_option(inst->ub, k,
						mainconfig.log_file);
			if (res) {
				goto error;
			}
			debug_method = 2;
			break;
		}
		/* FALL-THROUGH */

	case L_DST_NULL:
		debug_method = 3;
		break;

	default:
		debug_method = 4;
		break;
	}

	/* Now load the config file, which can override gleaned settings. */
	res = ub_ctx_config(inst->ub, inst->filename);
	if (res) goto error;

	/*
	 *	Check if the config file tried to use syslog.  Unbound
	 *	does not share syslog gracefully.
	 */
	strcpy(k, "use-syslog");
	res = ub_ctx_get_option(inst->ub, k, &optval);
	if (res || !optval) goto error;

	if (!strcmp(optval, "yes")) {
		char v[3];

		free(optval);

		WDEBUG("rlm_unbound (%s): Overriding syslog settings.", inst->name);
		strcpy(k, "use-syslog:");
		strcpy(v, "no");
		res = ub_ctx_set_option(inst->ub, k, v);
		if (res) goto error;

		if (debug_method == 2) {
			/* Reinstate the log file name JIC */
			strcpy(k, "logfile:");
			res = ub_ctx_set_option(inst->ub, k,
						mainconfig.log_file);
			if (res) goto error;
		}

	} else {
		if (optval) free(optval);
		strcpy(k, "logfile");

		res = ub_ctx_get_option(inst->ub, k, &optval);
		if (res) goto error;

		if (optval && strlen(optval)) {
			debug_method = 2;

		} else if (!debug_flag) {
			debug_method = 3;
		}

		if (optval) free(optval);
	}

	switch (debug_method) {
	case 1:
		/*
		 * We have an fd to log to.  And we've already attempted to
		 * dup it so libunbound doesn't close it on us.
		 */
		if (debug_fd == -1) {
			ERROR("rlm_unbound (%s): Could not dup fd", inst->name);
			goto error_nores;
		}

		inst->debug_stream = fdopen(debug_fd, "w");
		if (!inst->debug_stream) {
			ERROR("rlm_unbound (%s): error setting up log stream", inst->name);
			goto error_nores;
		}

		res = ub_ctx_debugout(inst->ub, inst->debug_stream);
		if (res) goto error;
		break;

	case 2:
		/* We gave libunbound a filename.  It is on its own now. */
		break;

	case 3:
		/* We tell libunbound not to log at all. */
		res = ub_ctx_debugout(inst->ub, NULL);
		if (res) goto error;
		break;

	case 4:
#ifdef HAVE_PTHREAD_H
		/*
		 *  Currently this wreaks havoc when running threaded, so just
		 *  turn logging off until that gets figured out.
		 */
		res = ub_ctx_debugout(inst->ub, NULL);
		if (res) goto error;
		break;
#else
		/*
		 *  We need to create a pipe, because libunbound does not
		 *  share syslog nicely.  Or the core added some new logsink.
		 */
		if (pipe(inst->logfd)) {
		error_pipe:
			EDEBUG("rlm_unbound (%s): Error setting up log pipes", inst->name);
			goto error_nores;
		}

		if ((fcntl(inst->logfd[0], F_SETFL, O_NONBLOCK) < 0) ||
		    (fcntl(inst->logfd[0], F_SETFD, FD_CLOEXEC) < 0)) {
			goto error_pipe;
		}

		/* Opaque to us when this can be closed, so we do not. */
		if (fcntl(inst->logfd[1], F_SETFL, O_NONBLOCK) < 0) {
			goto error_pipe;
		}

		inst->logstream[0] = fdopen(inst->logfd[0], "r");
		inst->logstream[1] = fdopen(inst->logfd[1], "w");

		if (!inst->logstream[0] || !inst->logstream[1]) {
			if (!inst->logstream[1]) {
				close(inst->logfd[1]);
			}

			if (!inst->logstream[0]) {
				close(inst->logfd[0]);
			}
			ERROR("rlm_unbound (%s): Error setting up log stream", inst->name);
			goto error_nores;
		}

		res = ub_ctx_debugout(inst->ub, inst->logstream[1]);
		if (res) goto error;

		if (!fr_event_fd_insert(inst->el, 0, inst->logfd[0],
					log_spew, inst)) {
			ERROR("rlm_unbound (%s): could not insert log fd", inst->name);
			goto error_nores;
		}

		inst->pipe_inuse = 1;
#endif
	default:
		break;
	}

	/*
	 *  Now we need to finalize the context.
	 *
	 *  There's no clean API to just finalize the context made public
	 *  in libunbound.  But we can trick it by trying to delete data
	 *  which as it happens fails quickly and quietly even though the
	 *  data did not exist.
	 */
	strcpy(k, "notar33lsite.foo123.nottld A 127.0.0.1");
	ub_ctx_data_remove(inst->ub, k);

	inst->fd = ub_fd(inst->ub);
	if (inst->fd >= 0) {
		if (!fr_event_fd_insert(inst->el, 0, inst->fd, ub_fd_handler, inst)) {
			ERROR("rlm_unbound (%s): could not insert async fd", inst->name);
			inst->fd = -1;
			goto error_nores;
		}

	}

	MEM(inst->xlat_a_name = talloc_typed_asprintf(inst, "%s-a", inst->name));
	MEM(inst->xlat_aaaa_name = talloc_typed_asprintf(inst, "%s-aaaa", inst->name));
	MEM(inst->xlat_ptr_name = talloc_typed_asprintf(inst, "%s-ptr", inst->name));

	if (xlat_register(inst->xlat_a_name, xlat_a, NULL, inst) ||
	    xlat_register(inst->xlat_aaaa_name, xlat_aaaa, NULL, inst) ||
	    xlat_register(inst->xlat_ptr_name, xlat_ptr, NULL, inst)) {
		ERROR("rlm_unbound (%s): Failed registering xlats", inst->name);
		xlat_unregister(inst->xlat_a_name, xlat_a, inst);
		xlat_unregister(inst->xlat_aaaa_name, xlat_aaaa, inst);
		xlat_unregister(inst->xlat_ptr_name, xlat_ptr, inst);
		goto error_nores;
	}
	return 0;

 error:
	ERROR("rlm_unbound (%s): %s", inst->name, ub_strerror(res));

 error_nores:
	if (debug_fd > -1) close(debug_fd);

	return -1;
}

static int mod_detach(UNUSED void *instance)
{
	rlm_unbound_t *inst = instance;

	xlat_unregister(inst->xlat_a_name, xlat_a, inst);
	xlat_unregister(inst->xlat_aaaa_name, xlat_aaaa, inst);
	xlat_unregister(inst->xlat_ptr_name, xlat_ptr, inst);

	if (inst->fd >= 0) {
		fr_event_fd_delete(inst->el, 0, inst->fd);
		if (inst->ub) {
			ub_process(inst->ub);
			/* This can hang/leave zombies currently
			 * see upstream bug #519
			 * ...so expect valgrind to complain with -m
			 */
#if 0
			ub_ctx_delete(inst->ub);
#endif
		}
	}

	if (inst->logstream[1]) {
		fclose(inst->logstream[1]);
	}

	if (inst->logstream[0]) {
		if (inst->pipe_inuse) {
			fr_event_fd_delete(inst->el, 0, inst->logfd[0]);
		}
		fclose(inst->logstream[0]);
	}

	if (inst->debug_stream) {
		fclose(inst->debug_stream);
	}

	return 0;
}

module_t rlm_unbound = {
	RLM_MODULE_INIT,
	"unbound",
	RLM_TYPE_THREAD_SAFE,		/* type */
	sizeof(rlm_unbound_t),
	module_config,
	mod_instantiate,		/* instantiation */
	mod_detach,			/* detach */
	/* This module does not directly interact with requests */
	{ NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
};
