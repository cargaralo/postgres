/*-------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/postinit.c
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/session.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_tablespace.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/postmaster.h"
#include "replication/slot.h"
#include "replication/slotsync.h"
#include "replication/walsender.h"
#include "storage/aio_subsys.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

static HeapTuple GetDatabaseTuple(const char *dbname);
static HeapTuple GetDatabaseTupleByOid(Oid dboid);
static void PerformAuthentication(Port *port);
static void CheckMyDatabase(const char *name, bool am_superuser, bool override_allow_connections);
static void ShutdownPostgres(int code, Datum arg);
static void StatementTimeoutHandler(void);
static void LockTimeoutHandler(void);
static void IdleInTransactionSessionTimeoutHandler(void);
static void TransactionTimeoutHandler(void);
static void IdleSessionTimeoutHandler(void);
static void IdleStatsUpdateTimeoutHandler(void);
static void ClientCheckTimeoutHandler(void);
static bool ThereIsAtLeastOneRole(void);
static void process_startup_options(Port *port, bool am_superuser);
static void process_settings(Oid databaseid, Oid roleid);


/*** InitPostgres support ***/


/*
 * GetDatabaseTuple -- fetch the pg_database row for a database
 *
 * This is used during backend startup when we don't yet have any access to
 * system catalogs in general.  In the worst case, we can seqscan pg_database
 * using nothing but the hard-wired descriptor that relcache.c creates for
 * pg_database.  In more typical cases, relcache.c was able to load
 * descriptors for both pg_database and its indexes from the shared relcache
 * cache file, and so we can do an indexscan.  criticalSharedRelcachesBuilt
 * tells whether we got the cached descriptors.
 */
static HeapTuple
GetDatabaseTuple(const char *dbname)
{
	HeapTuple	tuple;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];

	/*
	 * form a scan key
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));

	/*
	 * Open pg_database and fetch a tuple.  Force heap scan if we haven't yet
	 * built the critical shared relcache entries (i.e., we're starting up
	 * without a shared relcache cache file).
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(relation, DatabaseNameIndexId,
							  criticalSharedRelcachesBuilt,
							  NULL,
							  1, key);

	tuple = systable_getnext(scan);

	/* Must copy tuple before releasing buffer */
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	/* all done */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	return tuple;
}

/*
 * GetDatabaseTupleByOid -- as above, but search by database OID
 */
static HeapTuple
GetDatabaseTupleByOid(Oid dboid)
{
	HeapTuple	tuple;
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];

	/*
	 * form a scan key
	 */
	ScanKeyInit(&key[0],
				Anum_pg_database_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dboid));

	/*
	 * Open pg_database and fetch a tuple.  Force heap scan if we haven't yet
	 * built the critical shared relcache entries (i.e., we're starting up
	 * without a shared relcache cache file).
	 */
	relation = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(relation, DatabaseOidIndexId,
							  criticalSharedRelcachesBuilt,
							  NULL,
							  1, key);

	tuple = systable_getnext(scan);

	/* Must copy tuple before releasing buffer */
	if (HeapTupleIsValid(tuple))
		tuple = heap_copytuple(tuple);

	/* all done */
	systable_endscan(scan);
	table_close(relation, AccessShareLock);

	return tuple;
}


/*
 * PerformAuthentication -- authenticate a remote client
 *
 * returns: nothing.  Will not return at all if there's any failure.
 */
static void
PerformAuthentication(Port *port)
{
	/* This should be set already, but let's make sure */
	ClientAuthInProgress = true;	/* limit visibility of log messages */

	/*
	 * In EXEC_BACKEND case, we didn't inherit the contents of pg_hba.conf
	 * etcetera from the postmaster, and have to load them ourselves.
	 *
	 * FIXME: [fork/exec] Ugh.  Is there a way around this overhead?
	 */
#ifdef EXEC_BACKEND

	/*
	 * load_hba() and load_ident() want to work within the PostmasterContext,
	 * so create that if it doesn't exist (which it won't).  We'll delete it
	 * again later, in PostgresMain.
	 */
	if (PostmasterContext == NULL)
		PostmasterContext = AllocSetContextCreate(TopMemoryContext,
												  "Postmaster",
												  ALLOCSET_DEFAULT_SIZES);

	if (!load_hba())
	{
		/*
		 * It makes no sense to continue if we fail to load the HBA file,
		 * since there is no way to connect to the database in this case.
		 */
		ereport(FATAL,
		/* translator: %s is a configuration file */
				(errmsg("could not load %s", HbaFileName)));
	}

	if (!load_ident())
	{
		/*
		 * It is ok to continue if we fail to load the IDENT file, although it
		 * means that you cannot log in using any of the authentication
		 * methods that need a user name mapping. load_ident() already logged
		 * the details of error to the log.
		 */
	}
#endif

	/* Capture authentication start time for logging */
	conn_timing.auth_start = GetCurrentTimestamp();

	/*
	 * Set up a timeout in case a buggy or malicious client fails to respond
	 * during authentication.  Since we're inside a transaction and might do
	 * database access, we have to use the statement_timeout infrastructure.
	 */
	enable_timeout_after(STATEMENT_TIMEOUT, AuthenticationTimeout * 1000);

	/*
	 * Now perform authentication exchange.
	 */
	set_ps_display("authentication");
	ClientAuthentication(port); /* might not return, if failure */

	/*
	 * Done with authentication.  Disable the timeout, and log if needed.
	 */
	disable_timeout(STATEMENT_TIMEOUT, false);

	/* Capture authentication end time for logging */
	conn_timing.auth_end = GetCurrentTimestamp();

	if (log_connections & LOG_CONNECTION_AUTHORIZATION)
	{
		StringInfoData logmsg;

		initStringInfo(&logmsg);
		if (am_walsender)
			appendStringInfo(&logmsg, _("replication connection authorized: user=%s"),
							 port->user_name);
		else
			appendStringInfo(&logmsg, _("connection authorized: user=%s"),
							 port->user_name);
		if (!am_walsender)
			appendStringInfo(&logmsg, _(" database=%s"), port->database_name);

		if (port->application_name != NULL)
			appendStringInfo(&logmsg, _(" application_name=%s"),
							 port->application_name);

#ifdef USE_SSL
		if (port->ssl_in_use)
			appendStringInfo(&logmsg, _(" SSL enabled (protocol=%s, cipher=%s, bits=%d)"),
							 be_tls_get_version(port),
							 be_tls_get_cipher(port),
							 be_tls_get_cipher_bits(port));
#endif
#ifdef ENABLE_GSS
		if (port->gss)
		{
			const char *princ = be_gssapi_get_princ(port);

			if (princ)
				appendStringInfo(&logmsg,
								 _(" GSS (authenticated=%s, encrypted=%s, delegated_credentials=%s, principal=%s)"),
								 be_gssapi_get_auth(port) ? _("yes") : _("no"),
								 be_gssapi_get_enc(port) ? _("yes") : _("no"),
								 be_gssapi_get_delegation(port) ? _("yes") : _("no"),
								 princ);
			else
				appendStringInfo(&logmsg,
								 _(" GSS (authenticated=%s, encrypted=%s, delegated_credentials=%s)"),
								 be_gssapi_get_auth(port) ? _("yes") : _("no"),
								 be_gssapi_get_enc(port) ? _("yes") : _("no"),
								 be_gssapi_get_delegation(port) ? _("yes") : _("no"));
		}
#endif

		ereport(LOG, errmsg_internal("%s", logmsg.data));
		pfree(logmsg.data);
	}

	set_ps_display("startup");

	ClientAuthInProgress = false;	/* client_min_messages is active now */
}


/*
 * CheckMyDatabase -- fetch information from the pg_database entry for our DB
 */
static void
CheckMyDatabase(const char *name, bool am_superuser, bool override_allow_connections)
{
	HeapTuple	tup;
	Form_pg_database dbform;
	Datum		datum;
	bool		isnull;
	char	   *collate;
	char	   *ctype;

	/* Fetch our pg_database row normally, via syscache */
	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbform = (Form_pg_database) GETSTRUCT(tup);

	/* This recheck is strictly paranoia */
	if (strcmp(name, NameStr(dbform->datname)) != 0)
		ereport(FATAL,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" has disappeared from pg_database",
						name),
				 errdetail("Database OID %u now seems to belong to \"%s\".",
						   MyDatabaseId, NameStr(dbform->datname))));

	/*
	 * Check permissions to connect to the database.
	 *
	 * These checks are not enforced when in standalone mode, so that there is
	 * a way to recover from disabling all access to all databases, for
	 * example "UPDATE pg_database SET datallowconn = false;".
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * Check that the database is currently allowing connections.
		 * (Background processes can override this test and the next one by
		 * setting override_allow_connections.)
		 */
		if (!dbform->datallowconn && !override_allow_connections)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database \"%s\" is not currently accepting connections",
							name)));

		/*
		 * Check privilege to connect to the database.  (The am_superuser test
		 * is redundant, but since we have the flag, might as well check it
		 * and save a few cycles.)
		 */
		if (!am_superuser && !override_allow_connections &&
			object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
							ACL_CONNECT) != ACLCHECK_OK)
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for database \"%s\"", name),
					 errdetail("User does not have CONNECT privilege.")));

		/*
		 * Check connection limit for this database.  We enforce the limit
		 * only for regular backends, since other process types have their own
		 * PGPROC pools.
		 *
		 * There is a race condition here --- we create our PGPROC before
		 * checking for other PGPROCs.  If two backends did this at about the
		 * same time, they might both think they were over the limit, while
		 * ideally one should succeed and one fail.  Getting that to work
		 * exactly seems more trouble than it is worth, however; instead we
		 * just document that the connection limit is approximate.
		 */
		if (dbform->datconnlimit >= 0 &&
			AmRegularBackendProcess() &&
			!am_superuser &&
			CountDBConnections(MyDatabaseId) > dbform->datconnlimit)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("too many connections for database \"%s\"",
							name)));
	}

	/*
	 * OK, we're golden.  Next to-do item is to save the encoding info out of
	 * the pg_database tuple.
	 */
	SetDatabaseEncoding(dbform->encoding);
	/* Record it as a GUC internal option, too */
	SetConfigOption("server_encoding", GetDatabaseEncodingName(),
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	/* If we have no other source of client_encoding, use server encoding */
	SetConfigOption("client_encoding", GetDatabaseEncodingName(),
					PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);

	/* assign locale variables */
	datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datcollate);
	collate = TextDatumGetCString(datum);
	datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datctype);
	ctype = TextDatumGetCString(datum);

	/*
	 * Historcally, we set LC_COLLATE from datcollate, as well. That's no
	 * longer necessary because all collation behavior is handled through
	 * pg_locale_t.
	 */

	if (pg_perm_setlocale(LC_CTYPE, ctype) == NULL)
		ereport(FATAL,
				(errmsg("database locale is incompatible with operating system"),
				 errdetail("The database was initialized with LC_CTYPE \"%s\", "
						   " which is not recognized by setlocale().", ctype),
				 errhint("Recreate the database with another locale or install the missing locale.")));

	if (strcmp(ctype, "C") == 0 ||
		strcmp(ctype, "POSIX") == 0)
		database_ctype_is_c = true;

	init_database_collation();

	/*
	 * Check collation version.  See similar code in
	 * pg_newlocale_from_collation().  Note that here we warn instead of error
	 * in any case, so that we don't prevent connecting.
	 */
	datum = SysCacheGetAttr(DATABASEOID, tup, Anum_pg_database_datcollversion,
							&isnull);
	if (!isnull)
	{
		char	   *actual_versionstr;
		char	   *collversionstr;
		char	   *locale;

		collversionstr = TextDatumGetCString(datum);

		if (dbform->datlocprovider == COLLPROVIDER_LIBC)
			locale = collate;
		else
		{
			datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datlocale);
			locale = TextDatumGetCString(datum);
		}

		actual_versionstr = get_collation_actual_version(dbform->datlocprovider, locale);
		if (!actual_versionstr)
			/* should not happen */
			elog(WARNING,
				 "database \"%s\" has no actual collation version, but a version was recorded",
				 name);
		else if (strcmp(actual_versionstr, collversionstr) != 0)
			ereport(WARNING,
					(errmsg("database \"%s\" has a collation version mismatch",
							name),
					 errdetail("The database was created using collation version %s, "
							   "but the operating system provides version %s.",
							   collversionstr, actual_versionstr),
					 errhint("Rebuild all objects in this database that use the default collation and run "
							 "ALTER DATABASE %s REFRESH COLLATION VERSION, "
							 "or build PostgreSQL with the right library version.",
							 quote_identifier(name))));
	}

	ReleaseSysCache(tup);
}


/*
 * pg_split_opts -- split a string of options and append it to an argv array
 *
 * The caller is responsible for ensuring the argv array is large enough.  The
 * maximum possible number of arguments added by this routine is
 * (strlen(optstr) + 1) / 2.
 *
 * Because some option values can contain spaces we allow escaping using
 * backslashes, with \\ representing a literal backslash.
 */
void
pg_split_opts(char **argv, int *argcp, const char *optstr)
{
	StringInfoData s;

	initStringInfo(&s);

	while (*optstr)
	{
		bool		last_was_escape = false;

		resetStringInfo(&s);

		/* skip over leading space */
		while (isspace((unsigned char) *optstr))
			optstr++;

		if (*optstr == '\0')
			break;

		/*
		 * Parse a single option, stopping at the first space, unless it's
		 * escaped.
		 */
		while (*optstr)
		{
			if (isspace((unsigned char) *optstr) && !last_was_escape)
				break;

			if (!last_was_escape && *optstr == '\\')
				last_was_escape = true;
			else
			{
				last_was_escape = false;
				appendStringInfoChar(&s, *optstr);
			}

			optstr++;
		}

		/* now store the option in the next argv[] position */
		argv[(*argcp)++] = pstrdup(s.data);
	}

	pfree(s.data);
}

/*
 * Initialize MaxBackends value from config options.
 *
 * This must be called after modules have had the chance to alter GUCs in
 * shared_preload_libraries and before shared memory size is determined.
 *
 * Note that in EXEC_BACKEND environment, the value is passed down from
 * postmaster to subprocesses via BackendParameters in SubPostmasterMain; only
 * postmaster itself and processes not under postmaster control should call
 * this.
 */
void
InitializeMaxBackends(void)
{
	Assert(MaxBackends == 0);

	/* Note that this does not include "auxiliary" processes */
	MaxBackends = MaxConnections + autovacuum_worker_slots +
		max_worker_processes + max_wal_senders + NUM_SPECIAL_WORKER_PROCS;

	if (MaxBackends > MAX_BACKENDS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("too many server processes configured"),
				 errdetail("\"max_connections\" (%d) plus \"autovacuum_worker_slots\" (%d) plus \"max_worker_processes\" (%d) plus \"max_wal_senders\" (%d) must be less than %d.",
						   MaxConnections, autovacuum_worker_slots,
						   max_worker_processes, max_wal_senders,
						   MAX_BACKENDS - (NUM_SPECIAL_WORKER_PROCS - 1))));
}

/*
 * Initialize the number of fast-path lock slots in PGPROC.
 *
 * This must be called after modules have had the chance to alter GUCs in
 * shared_preload_libraries and before shared memory size is determined.
 */
void
InitializeFastPathLocks(void)
{
	/* Should be initialized only once. */
	Assert(FastPathLockGroupsPerBackend == 0);

	/*
	 * Based on the max_locks_per_transaction GUC, as that's a good indicator
	 * of the expected number of locks, figure out the value for
	 * FastPathLockGroupsPerBackend.  This must be a power-of-two.  We cap the
	 * value at FP_LOCK_GROUPS_PER_BACKEND_MAX and insist the value is at
	 * least 1.
	 *
	 * The default max_locks_per_transaction = 64 means 4 groups by default.
	 */
	FastPathLockGroupsPerBackend =
		Max(Min(pg_nextpower2_32(max_locks_per_xact) / FP_LOCK_SLOTS_PER_GROUP,
				FP_LOCK_GROUPS_PER_BACKEND_MAX), 1);

	/* Validate we did get a power-of-two */
	Assert(FastPathLockGroupsPerBackend ==
		   pg_nextpower2_32(FastPathLockGroupsPerBackend));
}

/*
 * Early initialization of a backend (either standalone or under postmaster).
 * This happens even before InitPostgres.
 *
 * This is separate from InitPostgres because it is also called by auxiliary
 * processes, such as the background writer process, which may not call
 * InitPostgres at all.
 */
void
BaseInit(void)
{
	Assert(MyProc != NULL);

	/*
	 * Initialize our input/output/debugging file descriptors.
	 */
	DebugFileOpen();

	/*
	 * Initialize file access. Done early so other subsystems can access
	 * files.
	 */
	InitFileAccess();

	/*
	 * Initialize statistics reporting. This needs to happen early to ensure
	 * that pgstat's shutdown callback runs after the shutdown callbacks of
	 * all subsystems that can produce stats (like e.g. transaction commits
	 * can).
	 */
	pgstat_initialize();

	/*
	 * Initialize AIO before infrastructure that might need to actually
	 * execute AIO.
	 */
	pgaio_init_backend();

	/* Do local initialization of storage and buffer managers */
	InitSync();
	smgrinit();
	InitBufferManagerAccess();

	/*
	 * Initialize temporary file access after pgstat, so that the temporary
	 * file shutdown hook can report temporary file statistics.
	 */
	InitTemporaryFileAccess();

	/*
	 * Initialize local buffers for WAL record construction, in case we ever
	 * try to insert XLOG.
	 */
	InitXLogInsert();

	/* Initialize lock manager's local structs */
	InitLockManagerAccess();

	/*
	 * Initialize replication slots after pgstat. The exit hook might need to
	 * drop ephemeral slots, which in turn triggers stats reporting.
	 */
	ReplicationSlotInitialize();
}


/* --------------------------------
 * InitPostgres
 *		Initialize POSTGRES.
 *
 * Parameters:
 *	in_dbname, dboid: specify database to connect to, as described below
 *	username, useroid: specify role to connect as, as described below
 *	flags:
 *	  - INIT_PG_LOAD_SESSION_LIBS to honor [session|local]_preload_libraries.
 *	  - INIT_PG_OVERRIDE_ALLOW_CONNS to connect despite !datallowconn.
 *	  - INIT_PG_OVERRIDE_ROLE_LOGIN to connect despite !rolcanlogin.
 *	out_dbname: optional output parameter, see below; pass NULL if not used
 *
 * The database can be specified by name, using the in_dbname parameter, or by
 * OID, using the dboid parameter.  Specify NULL or InvalidOid respectively
 * for the unused parameter.  If dboid is provided, the actual database
 * name can be returned to the caller in out_dbname.  If out_dbname isn't
 * NULL, it must point to a buffer of size NAMEDATALEN.
 *
 * Similarly, the role can be passed by name, using the username parameter,
 * or by OID using the useroid parameter.
 *
 * In bootstrap mode the database and username parameters are NULL/InvalidOid.
 * The autovacuum launcher process doesn't specify these parameters either,
 * because it only goes far enough to be able to read pg_database; it doesn't
 * connect to any particular database.  An autovacuum worker specifies a
 * database but not a username; conversely, a physical walsender specifies
 * username but not database.
 *
 * By convention, INIT_PG_LOAD_SESSION_LIBS should be passed in "flags" in
 * "interactive" sessions (including standalone backends), but not in
 * background processes such as autovacuum.  Note in particular that it
 * shouldn't be true in parallel worker processes; those have another
 * mechanism for replicating their leader's set of loaded libraries.
 *
 * We expect that InitProcess() was already called, so we already have a
 * PGPROC struct ... but it's not completely filled in yet.
 *
 * Note:
 *		Be very careful with the order of calls in the InitPostgres function.
 * --------------------------------
 */
void
InitPostgres(const char *in_dbname, Oid dboid,
			 const char *username, Oid useroid,
			 bits32 flags,
			 char *out_dbname)
{
	bool		bootstrap = IsBootstrapProcessingMode();
	bool		am_superuser;
	char	   *fullpath;
	char		dbname[NAMEDATALEN];
	int			nfree = 0;

	elog(DEBUG3, "InitPostgres");

	/*
	 * Add my PGPROC struct to the ProcArray.
	 *
	 * Once I have done this, I am visible to other backends!
	 */
	InitProcessPhase2();

	/* Initialize status reporting */
	pgstat_beinit();

	/*
	 * And initialize an entry in the PgBackendStatus array.  That way, if
	 * LWLocks or third-party authentication should happen to hang, it is
	 * possible to retrieve some information about what is going on.
	 */
	if (!bootstrap)
	{
		pgstat_bestart_initial();
		INJECTION_POINT("init-pre-auth", NULL);
	}

	/*
	 * Initialize my entry in the shared-invalidation manager's array of
	 * per-backend data.
	 */
	SharedInvalBackendInit(false);

	ProcSignalInit(MyCancelKey, MyCancelKeyLength);

	/*
	 * Also set up timeout handlers needed for backend operation.  We need
	 * these in every case except bootstrap.
	 */
	if (!bootstrap)
	{
		RegisterTimeout(DEADLOCK_TIMEOUT, CheckDeadLockAlert);
		RegisterTimeout(STATEMENT_TIMEOUT, StatementTimeoutHandler);
		RegisterTimeout(LOCK_TIMEOUT, LockTimeoutHandler);
		RegisterTimeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
						IdleInTransactionSessionTimeoutHandler);
		RegisterTimeout(TRANSACTION_TIMEOUT, TransactionTimeoutHandler);
		RegisterTimeout(IDLE_SESSION_TIMEOUT, IdleSessionTimeoutHandler);
		RegisterTimeout(CLIENT_CONNECTION_CHECK_TIMEOUT, ClientCheckTimeoutHandler);
		RegisterTimeout(IDLE_STATS_UPDATE_TIMEOUT,
						IdleStatsUpdateTimeoutHandler);
	}

	/*
	 * If this is either a bootstrap process or a standalone backend, start up
	 * the XLOG machinery, and register to have it closed down at exit. In
	 * other cases, the startup process is responsible for starting up the
	 * XLOG machinery, and the checkpointer for closing it down.
	 */
	if (!IsUnderPostmaster)
	{
		/*
		 * We don't yet have an aux-process resource owner, but StartupXLOG
		 * and ShutdownXLOG will need one.  Hence, create said resource owner
		 * (and register a callback to clean it up after ShutdownXLOG runs).
		 */
		CreateAuxProcessResourceOwner();

		StartupXLOG();
		/* Release (and warn about) any buffer pins leaked in StartupXLOG */
		ReleaseAuxProcessResources(true);
		/* Reset CurrentResourceOwner to nothing for the moment */
		CurrentResourceOwner = NULL;

		/*
		 * Use before_shmem_exit() so that ShutdownXLOG() can rely on DSM
		 * segments etc to work (which in turn is required for pgstats).
		 */
		before_shmem_exit(pgstat_before_server_shutdown, 0);
		before_shmem_exit(ShutdownXLOG, 0);
	}

	/*
	 * Initialize the relation cache and the system catalog caches.  Note that
	 * no catalog access happens here; we only set up the hashtable structure.
	 * We must do this before starting a transaction because transaction abort
	 * would try to touch these hashtables.
	 */
	RelationCacheInitialize();
	InitCatalogCache();
	InitPlanCache();

	/* Initialize portal manager */
	EnablePortalManager();

	/*
	 * Load relcache entries for the shared system catalogs.  This must create
	 * at least entries for pg_database and catalogs used for authentication.
	 */
	RelationCacheInitializePhase2();

	/*
	 * Set up process-exit callback to do pre-shutdown cleanup.  This is the
	 * one of the first before_shmem_exit callbacks we register; thus, this
	 * will be one the last things we do before low-level modules like the
	 * buffer manager begin to close down.  We need to have this in place
	 * before we begin our first transaction --- if we fail during the
	 * initialization transaction, as is entirely possible, we need the
	 * AbortTransaction call to clean up.
	 */
	before_shmem_exit(ShutdownPostgres, 0);

	/* The autovacuum launcher is done here */
	if (AmAutoVacuumLauncherProcess())
	{
		/* fill in the remainder of this entry in the PgBackendStatus array */
		pgstat_bestart_final();

		return;
	}

	/*
	 * Start a new transaction here before first access to db.
	 */
	if (!bootstrap)
	{
		/* statement_timestamp must be set for timeouts to work correctly */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();

		/*
		 * transaction_isolation will have been set to the default by the
		 * above.  If the default is "serializable", and we are in hot
		 * standby, we will fail if we don't change it to something lower.
		 * Fortunately, "read committed" is plenty good enough.
		 */
		XactIsoLevel = XACT_READ_COMMITTED;
	}

	/*
	 * Perform client authentication if necessary, then figure out our
	 * postgres user ID, and see if we are a superuser.
	 *
	 * In standalone mode, autovacuum worker processes and slot sync worker
	 * process, we use a fixed ID, otherwise we figure it out from the
	 * authenticated user name.
	 */
	if (bootstrap || AmAutoVacuumWorkerProcess() || AmLogicalSlotSyncWorkerProcess())
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
	}
	else if (!IsUnderPostmaster)
	{
		InitializeSessionUserIdStandalone();
		am_superuser = true;
		if (!ThereIsAtLeastOneRole())
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("no roles are defined in this database system"),
					 errhint("You should immediately run CREATE USER \"%s\" SUPERUSER;.",
							 username != NULL ? username : "postgres")));
	}
	else if (AmBackgroundWorkerProcess())
	{
		if (username == NULL && !OidIsValid(useroid))
		{
			InitializeSessionUserIdStandalone();
			am_superuser = true;
		}
		else
		{
			InitializeSessionUserId(username, useroid,
									(flags & INIT_PG_OVERRIDE_ROLE_LOGIN) != 0);
			am_superuser = superuser();
		}
	}
	else
	{
		/* normal multiuser case */
		Assert(MyProcPort != NULL);
		PerformAuthentication(MyProcPort);
		InitializeSessionUserId(username, useroid, false);
		/* ensure that auth_method is actually valid, aka authn_id is not NULL */
		if (MyClientConnectionInfo.authn_id)
			InitializeSystemUser(MyClientConnectionInfo.authn_id,
								 hba_authname(MyClientConnectionInfo.auth_method));
		am_superuser = superuser();
	}

	/* Report any SSL/GSS details for the session. */
	if (MyProcPort != NULL)
	{
		Assert(!bootstrap);

		pgstat_bestart_security();
	}

	/*
	 * Binary upgrades only allowed super-user connections
	 */
	if (IsBinaryUpgrade && !am_superuser)
	{
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to connect in binary upgrade mode")));
	}

	/*
	 * The last few regular connection slots are reserved for superusers and
	 * roles with privileges of pg_use_reserved_connections.  We do not apply
	 * these limits to background processes, since they all have their own
	 * pools of PGPROC slots.
	 *
	 * Note: At this point, the new backend has already claimed a proc struct,
	 * so we must check whether the number of free slots is strictly less than
	 * the reserved connection limits.
	 */
	if (AmRegularBackendProcess() && !am_superuser &&
		(SuperuserReservedConnections + ReservedConnections) > 0 &&
		!HaveNFreeProcs(SuperuserReservedConnections + ReservedConnections, &nfree))
	{
		if (nfree < SuperuserReservedConnections)
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("remaining connection slots are reserved for roles with the %s attribute",
							"SUPERUSER")));

		if (!has_privs_of_role(GetUserId(), ROLE_PG_USE_RESERVED_CONNECTIONS))
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("remaining connection slots are reserved for roles with privileges of the \"%s\" role",
							"pg_use_reserved_connections")));
	}

	/* Check replication permissions needed for walsender processes. */
	if (am_walsender)
	{
		Assert(!bootstrap);

		if (!has_rolreplication(GetUserId()))
			ereport(FATAL,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to start WAL sender"),
					 errdetail("Only roles with the %s attribute may start a WAL sender process.",
							   "REPLICATION")));
	}

	/*
	 * If this is a plain walsender only supporting physical replication, we
	 * don't want to connect to any particular database. Just finish the
	 * backend startup by processing any options from the startup packet, and
	 * we're done.
	 */
	if (am_walsender && !am_db_walsender)
	{
		/* process any options passed in the startup packet */
		if (MyProcPort != NULL)
			process_startup_options(MyProcPort, am_superuser);

		/* Apply PostAuthDelay as soon as we've read all options */
		if (PostAuthDelay > 0)
			pg_usleep(PostAuthDelay * 1000000L);

		/* initialize client encoding */
		InitializeClientEncoding();

		/* fill in the remainder of this entry in the PgBackendStatus array */
		pgstat_bestart_final();

		/* close the transaction we started above */
		CommitTransactionCommand();

		return;
	}

	/*
	 * Set up the global variables holding database id and default tablespace.
	 * But note we won't actually try to touch the database just yet.
	 *
	 * We take a shortcut in the bootstrap case, otherwise we have to look up
	 * the db's entry in pg_database.
	 */
	if (bootstrap)
	{
		dboid = Template1DbOid;
		MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
	}
	else if (in_dbname != NULL)
	{
		HeapTuple	tuple;
		Form_pg_database dbform;

		tuple = GetDatabaseTuple(in_dbname);
		if (!HeapTupleIsValid(tuple))
			ereport(FATAL,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", in_dbname)));
		dbform = (Form_pg_database) GETSTRUCT(tuple);
		dboid = dbform->oid;
	}
	else if (!OidIsValid(dboid))
	{
		/*
		 * If this is a background worker not bound to any particular
		 * database, we're done now.  Everything that follows only makes sense
		 * if we are bound to a specific database.  We do need to close the
		 * transaction we started before returning.
		 */
		if (!bootstrap)
		{
			pgstat_bestart_final();
			CommitTransactionCommand();
		}
		return;
	}

	/*
	 * Now, take a writer's lock on the database we are trying to connect to.
	 * If there is a concurrently running DROP DATABASE on that database, this
	 * will block us until it finishes (and has committed its update of
	 * pg_database).
	 *
	 * Note that the lock is not held long, only until the end of this startup
	 * transaction.  This is OK since we will advertise our use of the
	 * database in the ProcArray before dropping the lock (in fact, that's the
	 * next thing to do).  Anyone trying a DROP DATABASE after this point will
	 * see us in the array once they have the lock.  Ordering is important for
	 * this because we don't want to advertise ourselves as being in this
	 * database until we have the lock; otherwise we create what amounts to a
	 * deadlock with CountOtherDBBackends().
	 *
	 * Note: use of RowExclusiveLock here is reasonable because we envision
	 * our session as being a concurrent writer of the database.  If we had a
	 * way of declaring a session as being guaranteed-read-only, we could use
	 * AccessShareLock for such sessions and thereby not conflict against
	 * CREATE DATABASE.
	 */
	if (!bootstrap)
		LockSharedObject(DatabaseRelationId, dboid, 0, RowExclusiveLock);

	/*
	 * Recheck pg_database to make sure the target database hasn't gone away.
	 * If there was a concurrent DROP DATABASE, this ensures we will die
	 * cleanly without creating a mess.
	 */
	if (!bootstrap)
	{
		HeapTuple	tuple;
		Form_pg_database datform;

		tuple = GetDatabaseTupleByOid(dboid);
		if (HeapTupleIsValid(tuple))
			datform = (Form_pg_database) GETSTRUCT(tuple);

		if (!HeapTupleIsValid(tuple) ||
			(in_dbname && namestrcmp(&datform->datname, in_dbname)))
		{
			if (in_dbname)
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database \"%s\" does not exist", in_dbname),
						 errdetail("It seems to have just been dropped or renamed.")));
			else
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database %u does not exist", dboid)));
		}

		strlcpy(dbname, NameStr(datform->datname), sizeof(dbname));

		if (database_is_invalid_form(datform))
		{
			ereport(FATAL,
					errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("cannot connect to invalid database \"%s\"", dbname),
					errhint("Use DROP DATABASE to drop invalid databases."));
		}

		MyDatabaseTableSpace = datform->dattablespace;
		MyDatabaseHasLoginEventTriggers = datform->dathasloginevt;
		/* pass the database name back to the caller */
		if (out_dbname)
			strcpy(out_dbname, dbname);
	}

	/*
	 * Now that we rechecked, we are certain to be connected to a database and
	 * thus can set MyDatabaseId.
	 *
	 * It is important that MyDatabaseId only be set once we are sure that the
	 * target database can no longer be concurrently dropped or renamed.  For
	 * example, without this guarantee, pgstat_update_dbstats() could create
	 * entries for databases that were just dropped in the pgstat shutdown
	 * callback, which could confuse other code paths like the autovacuum
	 * scheduler.
	 */
	MyDatabaseId = dboid;

	/*
	 * Now we can mark our PGPROC entry with the database ID.
	 *
	 * We assume this is an atomic store so no lock is needed; though actually
	 * things would work fine even if it weren't atomic.  Anyone searching the
	 * ProcArray for this database's ID should hold the database lock, so they
	 * would not be executing concurrently with this store.  A process looking
	 * for another database's ID could in theory see a chance match if it read
	 * a partially-updated databaseId value; but as long as all such searches
	 * wait and retry, as in CountOtherDBBackends(), they will certainly see
	 * the correct value on their next try.
	 */
	MyProc->databaseId = MyDatabaseId;

	/*
	 * We established a catalog snapshot while reading pg_authid and/or
	 * pg_database; but until we have set up MyDatabaseId, we won't react to
	 * incoming sinval messages for unshared catalogs, so we won't realize it
	 * if the snapshot has been invalidated.  Assume it's no good anymore.
	 */
	InvalidateCatalogSnapshot();

	/*
	 * Now we should be able to access the database directory safely. Verify
	 * it's there and looks reasonable.
	 */
	fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);

	if (!bootstrap)
	{
		if (access(fullpath, F_OK) == -1)
		{
			if (errno == ENOENT)
				ereport(FATAL,
						(errcode(ERRCODE_UNDEFINED_DATABASE),
						 errmsg("database \"%s\" does not exist",
								dbname),
						 errdetail("The database subdirectory \"%s\" is missing.",
								   fullpath)));
			else
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not access directory \"%s\": %m",
								fullpath)));
		}

		ValidatePgVersion(fullpath);
	}

	SetDatabasePath(fullpath);
	pfree(fullpath);

	/*
	 * It's now possible to do real access to the system catalogs.
	 *
	 * Load relcache entries for the system catalogs.  This must create at
	 * least the minimum set of "nailed-in" cache entries.
	 */
	RelationCacheInitializePhase3();

	/* set up ACL framework (so CheckMyDatabase can check permissions) */
	initialize_acl();

	/*
	 * Re-read the pg_database row for our database, check permissions and set
	 * up database-specific GUC settings.  We can't do this until all the
	 * database-access infrastructure is up.  (Also, it wants to know if the
	 * user is a superuser, so the above stuff has to happen first.)
	 */
	if (!bootstrap)
		CheckMyDatabase(dbname, am_superuser,
						(flags & INIT_PG_OVERRIDE_ALLOW_CONNS) != 0);

	/*
	 * Now process any command-line switches and any additional GUC variable
	 * settings passed in the startup packet.   We couldn't do this before
	 * because we didn't know if client is a superuser.
	 */
	if (MyProcPort != NULL)
		process_startup_options(MyProcPort, am_superuser);

	/* Process pg_db_role_setting options */
	process_settings(MyDatabaseId, GetSessionUserId());

	/* Apply PostAuthDelay as soon as we've read all options */
	if (PostAuthDelay > 0)
		pg_usleep(PostAuthDelay * 1000000L);

	/*
	 * Initialize various default states that can't be set up until we've
	 * selected the active user and gotten the right GUC settings.
	 */

	/* set default namespace search path */
	InitializeSearchPath();

	/* initialize client encoding */
	InitializeClientEncoding();

	/* Initialize this backend's session state. */
	InitializeSession();

	/*
	 * If this is an interactive session, load any libraries that should be
	 * preloaded at backend start.  Since those are determined by GUCs, this
	 * can't happen until GUC settings are complete, but we want it to happen
	 * during the initial transaction in case anything that requires database
	 * access needs to be done.
	 */
	if ((flags & INIT_PG_LOAD_SESSION_LIBS) != 0)
		process_session_preload_libraries();

	/* fill in the remainder of this entry in the PgBackendStatus array */
	if (!bootstrap)
		pgstat_bestart_final();

	/* close the transaction we started above */
	if (!bootstrap)
		CommitTransactionCommand();
}

/*
 * Process any command-line switches and any additional GUC variable
 * settings passed in the startup packet.
 */
static void
process_startup_options(Port *port, bool am_superuser)
{
	GucContext	gucctx;
	ListCell   *gucopts;

	gucctx = am_superuser ? PGC_SU_BACKEND : PGC_BACKEND;

	/*
	 * First process any command-line switches that were included in the
	 * startup packet, if we are in a regular backend.
	 */
	if (port->cmdline_options != NULL)
	{
		/*
		 * The maximum possible number of commandline arguments that could
		 * come from port->cmdline_options is (strlen + 1) / 2; see
		 * pg_split_opts().
		 */
		char	  **av;
		int			maxac;
		int			ac;

		maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;

		av = (char **) palloc(maxac * sizeof(char *));
		ac = 0;

		av[ac++] = "postgres";

		pg_split_opts(av, &ac, port->cmdline_options);

		av[ac] = NULL;

		Assert(ac < maxac);

		(void) process_postgres_switches(ac, av, gucctx, NULL);
	}

	/*
	 * Process any additional GUC variable settings passed in startup packet.
	 * These are handled exactly like command-line variables.
	 */
	gucopts = list_head(port->guc_options);
	while (gucopts)
	{
		char	   *name;
		char	   *value;

		name = lfirst(gucopts);
		gucopts = lnext(port->guc_options, gucopts);

		value = lfirst(gucopts);
		gucopts = lnext(port->guc_options, gucopts);

		SetConfigOption(name, value, gucctx, PGC_S_CLIENT);
	}
}

/*
 * Load GUC settings from pg_db_role_setting.
 *
 * We try specific settings for the database/role combination, as well as
 * general for this database and for this user.
 */
static void
process_settings(Oid databaseid, Oid roleid)
{
	Relation	relsetting;
	Snapshot	snapshot;

	if (!IsUnderPostmaster)
		return;

	relsetting = table_open(DbRoleSettingRelationId, AccessShareLock);

	/* read all the settings under the same snapshot for efficiency */
	snapshot = RegisterSnapshot(GetCatalogSnapshot(DbRoleSettingRelationId));

	/* Later settings are ignored if set earlier. */
	ApplySetting(snapshot, databaseid, roleid, relsetting, PGC_S_DATABASE_USER);
	ApplySetting(snapshot, InvalidOid, roleid, relsetting, PGC_S_USER);
	ApplySetting(snapshot, databaseid, InvalidOid, relsetting, PGC_S_DATABASE);
	ApplySetting(snapshot, InvalidOid, InvalidOid, relsetting, PGC_S_GLOBAL);

	UnregisterSnapshot(snapshot);
	table_close(relsetting, AccessShareLock);
}

/*
 * Backend-shutdown callback.  Do cleanup that we want to be sure happens
 * before all the supporting modules begin to nail their doors shut via
 * their own callbacks.
 *
 * User-level cleanup, such as temp-relation removal and UNLISTEN, happens
 * via separate callbacks that execute before this one.  We don't combine the
 * callbacks because we still want this one to happen if the user-level
 * cleanup fails.
 */
static void
ShutdownPostgres(int code, Datum arg)
{
	/* Make sure we've killed any active transaction */
	AbortOutOfAnyTransaction();

	/*
	 * User locks are not released by transaction end, so be sure to release
	 * them explicitly.
	 */
	LockReleaseAll(USER_LOCKMETHOD, true);
}


/*
 * STATEMENT_TIMEOUT handler: trigger a query-cancel interrupt.
 */
static void
StatementTimeoutHandler(void)
{
	int			sig = SIGINT;

	/*
	 * During authentication the timeout is used to deal with
	 * authentication_timeout - we want to quit in response to such timeouts.
	 */
	if (ClientAuthInProgress)
		sig = SIGTERM;

#ifdef HAVE_SETSID
	/* try to signal whole process group */
	kill(-MyProcPid, sig);
#endif
	kill(MyProcPid, sig);
}

/*
 * LOCK_TIMEOUT handler: trigger a query-cancel interrupt.
 */
static void
LockTimeoutHandler(void)
{
#ifdef HAVE_SETSID
	/* try to signal whole process group */
	kill(-MyProcPid, SIGINT);
#endif
	kill(MyProcPid, SIGINT);
}

static void
TransactionTimeoutHandler(void)
{
	TransactionTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
IdleInTransactionSessionTimeoutHandler(void)
{
	IdleInTransactionSessionTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
IdleSessionTimeoutHandler(void)
{
	IdleSessionTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
IdleStatsUpdateTimeoutHandler(void)
{
	IdleStatsUpdateTimeoutPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

static void
ClientCheckTimeoutHandler(void)
{
	CheckClientConnectionPending = true;
	InterruptPending = true;
	SetLatch(MyLatch);
}

/*
 * Returns true if at least one role is defined in this database cluster.
 */
static bool
ThereIsAtLeastOneRole(void)
{
	Relation	pg_authid_rel;
	TableScanDesc scan;
	bool		result;

	pg_authid_rel = table_open(AuthIdRelationId, AccessShareLock);

	scan = table_beginscan_catalog(pg_authid_rel, 0, NULL);
	result = (heap_getnext(scan, ForwardScanDirection) != NULL);

	table_endscan(scan);
	table_close(pg_authid_rel, AccessShareLock);

	return result;
}
