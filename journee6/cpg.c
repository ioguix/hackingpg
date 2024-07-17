/*-------------------------------------------------------------------------
 * cpg.c: A Corosync background worker for PostgreSQL
 *
 * Its only goal is to detect which node is the primary and dynamically
 * set the primary conninfo in accordance.
 *
 * Base code inspired by src/test/modules/worker_spi/worker_spi.c
 * Documentation: https://www.postgresql.org/docs/current/bgworker.html
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 * Copyright (c) 2023-2024, Jehan-Guillaume de Rorthais
 *-------------------------------------------------------------------------
 */

#include <limits.h>					// INT_MAX
#include <corosync/cpg.h>			// corosync funcs

#include "postgres.h"				// definitions useful for PostgreSQL procs
#include "postmaster/bgworker.h"	// for a bgworker
#include "miscadmin.h"				// various variables, macros and funcs
#include "fmgr.h"					// needed for PG_MODULE_MAGIC
#include "storage/latch.h"			// for *Latch()
#include "utils/wait_event.h"		// for *Latch()
#include "utils/guc.h"				// for GUC
#include "postmaster/interrupt.h"	// for HandleMainLoopInterrupts() & co
#include "utils/ps_status.h"		// rename process
#include "access/xlog.h"			// for RecoveryInProgress()

#define CPG_GROUP_NAME "pgsql_group"

/* Required in all loadable module */
PG_MODULE_MAGIC;

/*
 * Declare main function of the bgworker and make sure it is callable from
 * external code using "PGDLLEXPORT"
 */
PGDLLEXPORT void cpg_main(Datum main_arg);

/*
 * Global variables
 */

/* Maximum interval between bgworker wakeups */
static int interval;
/* Remember recovery state */
static bool in_recovery;
/* Remember the number of known members */
static uint32_t members;
/* Local node id as set in corosync config */
static uint32_t MyNodeID;

/*
 * Functions
 */

/*
 * Update processus title
 */
static void
update_ps_display()
{
	char ps_display[128];

	if (in_recovery)
		snprintf(ps_display, 128, "[%u] Hello!", members);
	else
		snprintf(ps_display, 128, "[%u] I'm the primary!", members);

	set_ps_display(ps_display);
}

/*
 * Signal handler for SIGTERM
 *
 * Say good bye!
 */
static void
cpg_sigterm(SIGNAL_ARGS)
{
	elog(LOG, "[cpg] …and leaving");

	exit(0);
}

/*
 * Corosync callbacks. See cpg_initialize(3) for reference.
 *
 */

/*
 * Implementation of the CPG config callback. Called when members join or
 * leave the group.
 */
static void
cs_config_cb(cpg_handle_t gh,
			 const struct cpg_name *groupName,
			 const struct cpg_address *member_list,
			 size_t member_list_entries,
			 const struct cpg_address *left_list,
			 size_t left_list_entries,
			 const struct cpg_address *joined_list,
			 size_t joined_list_entries)
{
	unsigned int i;
	StringInfoData msg;

	/* Update number of members in the process title */
	members = member_list_entries;
	update_ps_display();

	/* Sum up current members */
	initStringInfo(&msg);
	for (i = 0; i < member_list_entries; i++)
		appendStringInfo(&msg, ", %d/%d", member_list[i].nodeid,
						 member_list[i].pid);

	elog(LOG, "[cpg] %lu join, %lu left, procs in group now: %s",
		 joined_list_entries, left_list_entries, msg.data+2);

	pfree(msg.data);

	/* Did I left the group ? */
	if (left_list_entries
		&& (pid_t)left_list[0].pid == MyProcPid
		&& left_list[0].nodeid == MyNodeID)
	{
		/* …then quit */
		elog(FATAL, "[cpg] I left the closed process group!");
	}
}

/*
 * The bgworker main function.
 *
 * This is called from the bgworker backend freshly forked and setup by
 * postmaster.
 */
void
cpg_main(Datum main_arg)
{
	cs_error_t rc;
	/* Structure holding the CPG setup and callback */
	cpg_model_v1_data_t model_data;
	/* Group name in corosync internal format */
	struct cpg_name cpg_group;
	/* Structure holding the group handle */
	cpg_handle_t gh;
	int32_t cpg_fd;

	/*
	 * By default, signals are blocked when the background worker starts.
	 * This allows to set up some signal handlers during the worker
	 * startup.
	 */

	/*
	 * Install signal handlers
	 */
	/* Leave gracefully on shutdown */
	pqsignal(SIGTERM, cpg_sigterm);
	/* Core facility to handle config reload */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);

	/* We can now unblock signals */
	BackgroundWorkerUnblockSignals();

	/*
	 * GUC declarations
	 */

	/* interval */
	DefineCustomIntVariable("cpg.interval",	// name
							"Defines the maximal interval in seconds between "
							"wakeups",		// description
							NULL,			// long description
							&interval,		// variable to set
							10,				// default value
							1,				// min value
							INT_MAX / 1000,	// max value
							PGC_SIGHUP,		// context
							GUC_UNIT_S,		// flags
							NULL,			// check hook
							NULL,			// assign hook
							NULL);			// show hook

	/*
	 * Lock namespace "cpg". Forbid creating any other GUC after here. Useful
	 * to remove unknown or mispelled GUCs from this domain.
	 */
#if PG_VERSION_NUM >= 150000
	MarkGUCPrefixReserved("cpg");
#else
	/* Note that this still exists in v15+ as a macro calling
	 * MarkGUCPrefixReserved */
	EmitWarningsOnPlaceholders("cpg");
#endif

	elog(LOG, "[cpg] Starting…");

	/* Set initial status and proc title (in this order) */
	in_recovery = RecoveryInProgress();

	update_ps_display();

	/*
	 * Corosync initialization and membership
	 */

	/* Corosync callbacks setup */
	model_data.flags = CPG_MODEL_V1_DELIVER_INITIAL_TOTEM_CONF;
	model_data.cpg_deliver_fn = NULL; /* TODO */
	model_data.cpg_confchg_fn = cs_config_cb;
	/* We choose to not implement the totem callback as it is not relevent */
	model_data.cpg_totem_confchg_fn = NULL;

	/* Initialize the connection */
	rc = cpg_model_initialize(&gh, CPG_MODEL_V1,
							  (cpg_model_data_t *) &model_data, NULL);
	if (rc != CS_OK)
		elog(FATAL, "[cpg] could not init the cpg handle: %d", rc);

	/* Initialize the group name structure */
	strncpy(cpg_group.value, CPG_GROUP_NAME, CPG_MAX_NAME_LENGTH);
	cpg_group.length = strlen(CPG_GROUP_NAME);

	/* Try to join the Close Process Group! */
	rc = cpg_join(gh, &cpg_group);
	if (rc == CS_OK)
		elog(LOG, "[cpg] joined group '" CPG_GROUP_NAME);
	else if (rc == CS_ERR_INVALID_PARAM)
		elog(FATAL, "[cpg] the handle is already joined to a group");
	else
		elog(FATAL, "[cpg] could not join the close process group: %d", rc);

	/* Get the local node id. This is needed to identify our own messages */
	rc = cpg_local_get(gh, &MyNodeID);
	if (rc != CS_OK)
		elog(FATAL, "[cpg] failed to get local nodeid: %d", rc);

	/* Get the file descriptor used for the group communication. We watch it in
	 * the event loop to detect incoming events. */
	rc = cpg_fd_get(gh, &cpg_fd);
	if (rc != CS_OK)
		elog(FATAL, "[cpg] failed to get the CPG file descriptor: %d", rc);

	/* Event loop */
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* Check if an event is waiting to be processed, without blocking the
		 * loop */
		rc = cpg_dispatch(gh, CS_DISPATCH_ONE_NONBLOCKING);
		if (rc == CS_OK)
			elog(DEBUG1, "[cpg] dispatched one event");
		else if (rc != CS_ERR_TRY_AGAIN)
			/* Using CS_DISPATCH_ONE_NONBLOCKING, cpg_dispatch returns
			 * CS_ERR_TRY_AGAIN when no event are waiting to be processed. This
			 * is not a failure to catch */
			elog(FATAL, "[cpg] dispatching callback failed: %d", rc);

		if (RecoveryInProgress() != in_recovery)
		{
			elog(LOG, "[cpg] I've been promoted!");
			in_recovery = RecoveryInProgress();
			update_ps_display();
		}
		else
			elog(LOG, "[cpg] Hi!");

		/* Facility from core. checks if some smiple and common interrupts,
		 * including SIGHUP to reload conf files, and process them. */
		HandleMainLoopInterrupts();

		/* Wait for an event or timeout */
		WaitLatchOrSocket(MyLatch,
						  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH |
						  WL_SOCKET_READABLE,
						  cpg_fd,
						  interval*1000, /* convert to milliseconds */
						  PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}

	elog(LOG, "[cpg] oops, the event loop broke");

	exit(1);
}

/*
 * Module init.
 *
 * This is called from the Postmaster process.
 */
void
_PG_init(void)
{
	BackgroundWorker worker;

	/*
	 * This bgw can only be loaded from shared_preload_libraries.
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Setup the bgworker
	 */

	/* Memory init */
	memset(&worker, 0, sizeof(worker));

	/* The bgw _currently_ don't need shmem access neither database connection.
	 * However, documentation states that BGWORKER_SHMEM_ACCESS is always
	 * required. If omitted, the bgw is fully ignored by postmaster. */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;

	/* Start the bgworker as soon as a consistant state has been reached */
	worker.bgw_start_time = BgWorkerStart_ConsistentState;

	/* Set the name of the bgwriter library */
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "cpg");

	/* Set the name of main function to start the bgworker */
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "cpg_main");

	/* Set the name of the bgworker */
	snprintf(worker.bgw_name, BGW_MAXLEN, "cpg");

	/* For this demo, just never come back if the bgw exits */
	worker.bgw_restart_time = BGW_NEVER_RESTART;

	/* No arg to pass to the bgworker's main function */
	worker.bgw_main_arg = (Datum) 0;

	/* This bgworker is not loaded dynamicaly, no backend to notify after
	 * successful start */
	worker.bgw_notify_pid = 0;

	/*
	 * Register the bgworker with the postmaster
	 */
	RegisterBackgroundWorker(&worker);
}
