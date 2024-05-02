/*-------------------------------------------------------------------------
 *
 * cpg.c: A Corosync background worker for PostgreSQL
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 * Copyright (c) 2023, Jehan-Guillaume de Rorthais
 *-------------------------------------------------------------------------
 */

#include <unistd.h>
#include <limits.h>

#include "postgres.h"

/* Some string utility funcs */
#include "lib/stringinfo.h"

/* For a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h" // pour proc_exit()
#include "access/xlog.h" // pour RecoeveryInProgress()

/* There is a GUC */
#include "utils/guc.h"

/* rename process */
#include "utils/ps_status.h"

/* For Corosync */
#include <corosync/cpg.h>

PG_MODULE_MAGIC;

void _PG_init(void);
PGDLLEXPORT void cpg_main(Datum main_arg) __attribute__((noreturn));

/*************************************************************
 * Functions, callbacks, structures et variables pour corosync
 */

static void message_cb ( cpg_handle_t gh, const struct cpg_name *groupName,
				  uint32_t nodeid, uint32_t pid, void *msg,
				  size_t msg_len );

static void config_cb (
	cpg_handle_t handle,
	const struct cpg_name *groupName,
	const struct cpg_address *member_list, size_t member_list_entries,
	const struct cpg_address *left_list, size_t left_list_entries,
	const struct cpg_address *joined_list, size_t joined_list_entries );

static void totem_config_cb (
	cpg_handle_t handle,
	struct cpg_ring_id ring_id,
	uint32_t member_list_entries,
	const uint32_t *member_list );

static void cpg_sigterm(SIGNAL_ARGS);

static cpg_model_v1_data_t model_data = { /* callbacks setup */
	.cpg_deliver_fn =            message_cb,
	.cpg_confchg_fn =            config_cb,
	.cpg_totem_confchg_fn =      totem_config_cb,
	.flags =                     CPG_MODEL_V1_DELIVER_INITIAL_TOTEM_CONF,
};

static char	       *group_name = NULL;	/* The CPG group name to join */
static cpg_handle_t	gh;
static struct cpg_name	cpg_group;
static uint32_t		mynodeid;

static const char * cs_get_errmsg(int rc)
{
	switch (rc) {
		case CS_ERR_TRY_AGAIN:
			return "Resource temporarily unavailable";
		case CS_ERR_INVALID_PARAM:
			return "Invalid argument";
		case CS_ERR_ACCESS:
			return "Permission denied";
		case CS_ERR_LIBRARY:
			return "The connection failed";
		case CS_ERR_INTERRUPT:
			return "System call interrupted by a signal";
		case CS_ERR_NOT_SUPPORTED:
			return "The requested protocol/functionality not supported";
		case CS_ERR_MESSAGE_ERROR:
			return "Incorrect auth message received";
		case CS_ERR_NO_MEMORY:
			return "Not enough memory to complete the requested task";
	}
	return "Error not documented!";
}

void totem_config_cb (
	cpg_handle_t handle,
	struct cpg_ring_id ring_id,
	uint32_t member_list_entries,
	const uint32_t *member_list
) {
	unsigned int i;
	StringInfoData msg;

	initStringInfo(&msg);

	elog(LOG, "totem_config_cb: ringid (%d.%ld)",
		ring_id.nodeid, ring_id.seq);


	appendStringInfo(&msg, "%d", member_list[0]);
	for (i = 1; i < member_list_entries; i++) {
		appendStringInfo(&msg, ", %d", member_list[i]);
	}
	elog(LOG, "totem_config_cb: active processors: %s", msg.data);
	pfree(msg.data);
}

void config_cb (
	cpg_handle_t handle,
	const struct cpg_name *groupName,
	const struct cpg_address *member_list, size_t member_list_entries,
	const struct cpg_address *left_list, size_t left_list_entries,
	const struct cpg_address *joined_list, size_t joined_list_entries )
{
	unsigned int	i;
	StringInfoData msg;

	elog(LOG, "cpg: config_cb: (%d/%d) config changed in group '%s'",
		 mynodeid, MyProcPid, group_name);

	for (i = 0; i < joined_list_entries; i++) {
		elog(LOG, "cpg: joined %d/%d reason: %d",
			 joined_list[i].nodeid, joined_list[i].pid,
			 joined_list[i].reason);
	}

	for (i = 0; i < left_list_entries; i++) {
		elog(LOG, "cpg: left %d/%d reason: %d",
			 left_list[i].nodeid, left_list[i].pid, left_list[i].reason);
	}

	initStringInfo(&msg);
	for (i = 0; i < member_list_entries; i++) {
		appendStringInfo(&msg, ", %d/%d",
						 member_list[i].nodeid, member_list[i].pid);
	}
	elog(LOG, "cpg: nodes in group now: %s", msg.data+2);

	if (member_list_entries == 3 && !RecoveryInProgress()) {
		struct iovec iovec;
		char hostname[HOST_NAME_MAX];
		int	rc;

		gethostname(hostname, HOST_NAME_MAX);

		resetStringInfo(&msg);
		appendStringInfo(&msg, "primary=%s", hostname);

		iovec.iov_base = (void *) msg.data;
		iovec.iov_len = msg.len +1;

		rc = cpg_mcast_joined(gh, CPG_TYPE_FIFO, &iovec, 1);
		if ( rc == CS_OK )
			elog(DEBUG1, "cpg: config_cb: message sent: %s", msg.data);
		else
			elog(FATAL, "cpg: config_cb: could not send message: %s",
				 cs_get_errmsg(rc));
	}

	pfree(msg.data);

	/* Did I left the group ? */
	if ( left_list_entries
		 && (pid_t)left_list[0].pid == MyProcPid
		 && left_list[0].nodeid == mynodeid
	) {
		elog(FATAL, "We left the closed process group");
		/* never reached*/
	}
}

void message_cb ( cpg_handle_t gh, const struct cpg_name *groupName,
						 uint32_t nodeid, uint32_t pid, void *msg,
						 size_t msg_len )
{
	if ( nodeid == mynodeid && pid == MyProcPid )
		elog( DEBUG1, "cpg: message_cb: ignoring own message" );
	else
	{
		char	primary_host[HOST_NAME_MAX];

		elog( LOG, "cpg: message_cb: message (len=%lu) from %d/%d: '%s'",
			  (unsigned long int) msg_len, nodeid, pid,
			  (const char *) msg );

		if (sscanf((const char *) msg, "primary=%s", primary_host) == 1)
		{
			StringInfoData val;
			
			initStringInfo(&val);
			appendStringInfo(&val, "host=%s", primary_host);

			elog( LOG, "cpg: message_cb: I have a primary: %s", primary_host );
			set_config_option("primary_conninfo", val.data, PGC_SIGHUP,
							  PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false );

			if(kill(PostmasterPid, SIGHUP))
			{
				elog(FATAL,
					 "cpg: message_cb: failed to send signal to postmaster");
                /* never reached */
			}

			pfree(val.data);
		}
	}
}

/*
 * Signal handler for SIGTERM
 */
static void
cpg_sigterm(SIGNAL_ARGS)
{
	cs_error_t	rc;

	elog(LOG, "cpg: received signal to shut down");

	rc = cpg_leave(gh, &cpg_group);

	if ( rc == CS_OK )
		elog(LOG, "cpg: left group '%s'", group_name);
	else
		elog(FATAL, "cpg: leaving group '%s' failed: %s",
			 group_name, cs_get_errmsg(rc));

	rc = cpg_finalize(gh);

	if ( rc == CS_OK )
		elog(DEBUG1, "cpg: handle finalized");
	else
		elog(FATAL, "cpg: finalizing handle failed: %s", cs_get_errmsg(rc));

	proc_exit(0);
}

void
cpg_main(Datum main_arg)
{
	char	ps_display[128];
	cs_error_t	rc;

	pqsignal(SIGTERM, cpg_sigterm);

	BackgroundWorkerUnblockSignals();

	snprintf(ps_display, 128, "connecting to closed process group '%s'", group_name);
	set_ps_display(ps_display);

	rc = cpg_model_initialize(&gh, CPG_MODEL_V1,
							  (cpg_model_data_t *)&model_data, NULL);
	if ( rc != CS_OK )
		elog(FATAL, "cpg: could not init the cpg handle: %s",
			 cs_get_errmsg(rc));

	strncpy(cpg_group.value, group_name, CPG_MAX_NAME_LENGTH);
	cpg_group.length = strlen(group_name);

	rc = cpg_join(gh, &cpg_group);
	if ( rc == CS_OK )
	{
		snprintf(ps_display, 128, "joined closed process group '%s'", group_name);
		set_ps_display(ps_display);
	}
	else if ( rc == CS_ERR_INVALID_PARAM )
	{
		elog(FATAL, "cpg: the handle is already joined to a group");
	}
	else
		elog(FATAL, "cpg: could not join the close process group: %s",
			 cs_get_errmsg(rc));

	rc = cpg_local_get(gh, &mynodeid);
	if(rc != CS_OK) {
		elog(FATAL, "cpg: config_cb: failed to get local nodeid: %s", cs_get_errmsg(rc));
		/* never reached */
	}

	/* Event loop */
	for (;;)
	{
		rc = cpg_dispatch(gh, CS_DISPATCH_ONE);

		if ( rc == CS_OK )
			elog(DEBUG1, "cpg: dispatched one event");
		else
			elog(FATAL, "cpg: dispatching callback failed: %s",
				 cs_get_errmsg(rc));
	}
}

/****************************
 * Point d'entrée du bgworker
 */

void
_PG_init(void)
{
	BackgroundWorker worker;

	/* Déclaration du GUC */

	DefineCustomStringVariable("cpg.group_name",
							   "The CPG group name to join",
							   NULL,
							   &group_name,
							   "pgsql_group", PGC_SIGHUP, 0, NULL, NULL, NULL);

	/*
	 * Reserve le namespace "cpg", empêchant la création de toute autre GUC
	 * à partir d'ici.
	 */
	// MarkGUCPrefixReserved("cpg"); /* v15+ */
	EmitWarningsOnPlaceholders("cpg");

	/*
	 * Ce bgw ne peut être chargé que depuis shared_preload_libraries.
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Configure, puis enregistre le bgworker auprès du postmaster
	 */
	memset(&worker, 0, sizeof(worker));

	/* FIXME: a-t-on besoin de SHMEM_ACCESS ? */
	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState; // BgWorkerStart_PostmasterStart;

	snprintf(worker.bgw_library_name, BGW_MAXLEN, "cpg");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "cpg_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "cpg");
	/* FIXME: devons nous redémarrer automatiquement ? */
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main_arg = (Datum) 0;
	worker.bgw_notify_pid = 0;

	RegisterBackgroundWorker(&worker);
}

