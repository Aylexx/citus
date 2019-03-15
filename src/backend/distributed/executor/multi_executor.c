/*-------------------------------------------------------------------------
 *
 * multi_executor.c
 *
 * Entrypoint into distributed query execution.
 *
 * Copyright (c) 2012-2016, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "distributed/citus_custom_scan.h"
#include "distributed/commands/multi_copy.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/insert_select_executor.h"
#include "distributed/insert_select_planner.h"
#include "distributed/multi_executor.h"
#include "distributed/multi_master_planner.h"
#include "distributed/distributed_planner.h"
#include "distributed/multi_router_executor.h"
#include "distributed/multi_router_planner.h"
#include "distributed/multi_resowner.h"
#include "distributed/multi_server_executor.h"
#include "distributed/resource_lock.h"
#include "distributed/worker_protocol.h"
#include "executor/execdebug.h"
#include "commands/copy.h"
#include "nodes/makefuncs.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"


/*
 * Controls the connection type for multi shard modifications, DDLs
 * TRUNCATE and real-time SELECT queries.
 */
int MultiShardConnectionType = PARALLEL_CONNECTION;
bool WritableStandbyCoordinator = false;


/* local function forward declarations */
static bool IsCitusPlan(Plan *plan);
static bool IsCitusCustomScan(Plan *plan);
static Relation StubRelation(TupleDesc tupleDescriptor);
static bool AlterTableConstraintCheck(QueryDesc *queryDesc);

/*
 * CitusExecutorStart is the ExecutorStart_hook that gets called when
 * Postgres prepares for execution or EXPLAIN.
 */
void
CitusExecutorStart(QueryDesc *queryDesc, int eflags)
{
	PlannedStmt *plannedStmt = queryDesc->plannedstmt;

	/*
	 * We cannot modify XactReadOnly on Windows because it is not
	 * declared with PGDLLIMPORT.
	 */
#ifndef WIN32
	if (RecoveryInProgress() && WritableStandbyCoordinator &&
		IsCitusPlan(plannedStmt->planTree))
	{
		PG_TRY();
		{
			/*
			 * To enable writes from a hot standby we cheat our way through
			 * the checks in standard_ExecutorStart by temporarily setting
			 * XactReadOnly to false.
			 */
			XactReadOnly = false;
			standard_ExecutorStart(queryDesc, eflags);
			XactReadOnly = true;
		}
		PG_CATCH();
		{
			XactReadOnly = true;
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
#endif
	{
		standard_ExecutorStart(queryDesc, eflags);
	}
}


/*
 * CitusExecutorRun is the ExecutorRun_hook that gets called when postgres
 * executes a query.
 */
void
CitusExecutorRun(QueryDesc *queryDesc,
				 ScanDirection direction, uint64 count, bool execute_once)
{
	DestReceiver *dest = queryDesc->dest;
	int originalLevel = FunctionCallLevel;

	if (dest->mydest == DestSPI)
	{
		/*
		 * If the query runs via SPI, we assume we're in a function call
		 * and we should treat statements as part of a bigger transaction.
		 * We reset this counter to 0 in the abort handler.
		 */
		FunctionCallLevel++;
	}

	/*
	 * Disable execution of ALTER TABLE constraint validation queries. These
	 * constraints will be validated in worker nodes, so running these queries
	 * from the coordinator would be redundant.
	 *
	 * For example, ALTER TABLE ... ATTACH PARTITION checks that the new
	 * partition doesn't violate constraints of the parent table, which
	 * might involve running some SELECT queries.
	 *
	 * Ideally we'd completely skip these checks in the coordinator, but we don't
	 * have any means to tell postgres to skip the checks. So the best we can do is
	 * to not execute the queries and return an empty result set, as if this table has
	 * no rows, so no constraints will be violated.
	 */
	if (AlterTableConstraintCheck(queryDesc))
	{
		EState *estate = queryDesc->estate;

		estate->es_processed = 0;
		estate->es_lastoid = InvalidOid;

		/* start and shutdown tuple receiver to simulate empty result */
		dest->rStartup(queryDesc->dest, CMD_SELECT, queryDesc->tupDesc);
		dest->rShutdown(dest);
	}
	else
	{
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
	}

	if (dest->mydest == DestSPI)
	{
		/*
		 * Restore the original value. It is not sufficient to decrease
		 * the value because exceptions might cause us to go back a few
		 * levels at once.
		 */
		FunctionCallLevel = originalLevel;
	}
}


/*
 * IsCitusPlan returns whether a Plan contains a CustomScan generated by Citus
 * by recursively walking through the plan tree.
 */
static bool
IsCitusPlan(Plan *plan)
{
	if (plan == NULL)
	{
		return false;
	}

	if (IsCitusCustomScan(plan))
	{
		return true;
	}

	if (plan->lefttree != NULL && IsCitusPlan(plan->lefttree))
	{
		return true;
	}

	if (plan->righttree != NULL && IsCitusPlan(plan->righttree))
	{
		return true;
	}

	return false;
}


/*
 * IsCitusCustomScan returns whether Plan node is a CustomScan generated by Citus.
 */
static bool
IsCitusCustomScan(Plan *plan)
{
	CustomScan *customScan = NULL;
	Node *privateNode = NULL;

	if (plan == NULL)
	{
		return false;
	}

	if (!IsA(plan, CustomScan))
	{
		return false;
	}

	customScan = (CustomScan *) plan;
	if (list_length(customScan->custom_private) == 0)
	{
		return false;
	}

	privateNode = (Node *) linitial(customScan->custom_private);
	if (!CitusIsA(privateNode, DistributedPlan))
	{
		return false;
	}

	return true;
}


/*
 * ReturnTupleFromTuplestore reads the next tuple from the tuple store of the
 * given Citus scan node and returns it. It returns null if all tuples are read
 * from the tuple store.
 */
TupleTableSlot *
ReturnTupleFromTuplestore(CitusScanState *scanState)
{
	Tuplestorestate *tupleStore = scanState->tuplestorestate;
	TupleTableSlot *resultSlot = NULL;
	ScanDirection scanDirection = NoMovementScanDirection;
	bool forwardScanDirection = true;

	if (tupleStore == NULL)
	{
		return NULL;
	}

	scanDirection = scanState->customScanState.ss.ps.state->es_direction;
	Assert(ScanDirectionIsValid(scanDirection));

	if (ScanDirectionIsBackward(scanDirection))
	{
		forwardScanDirection = false;
	}

	resultSlot = scanState->customScanState.ss.ps.ps_ResultTupleSlot;
	tuplestore_gettupleslot(tupleStore, forwardScanDirection, false, resultSlot);

	return resultSlot;
}


/*
 * Load data collected by real-time or task-tracker executors into the tuplestore
 * of CitusScanState. For that, we first create a tuple store, and then copy the
 * files one-by-one into the tuple store.
 *
 * Note that in the long term it'd be a lot better if Multi*Execute() directly
 * filled the tuplestores, but that's a fair bit of work.
 */
void
LoadTuplesIntoTupleStore(CitusScanState *citusScanState, Job *workerJob)
{
	CustomScanState customScanState = citusScanState->customScanState;
	List *workerTaskList = workerJob->taskList;
	TupleDesc tupleDescriptor = NULL;
	ListCell *workerTaskCell = NULL;
	bool randomAccess = true;
	bool interTransactions = false;
	char *copyFormat = "text";

	tupleDescriptor = customScanState.ss.ps.ps_ResultTupleSlot->tts_tupleDescriptor;

	Assert(citusScanState->tuplestorestate == NULL);
	citusScanState->tuplestorestate =
		tuplestore_begin_heap(randomAccess, interTransactions, work_mem);

	if (BinaryMasterCopyFormat)
	{
		copyFormat = "binary";
	}

	foreach(workerTaskCell, workerTaskList)
	{
		Task *workerTask = (Task *) lfirst(workerTaskCell);
		StringInfo jobDirectoryName = NULL;
		StringInfo taskFilename = NULL;

		jobDirectoryName = MasterJobDirectoryName(workerTask->jobId);
		taskFilename = TaskFilename(jobDirectoryName, workerTask->taskId);

		ReadFileIntoTupleStore(taskFilename->data, copyFormat, tupleDescriptor,
							   citusScanState->tuplestorestate);
	}

	tuplestore_donestoring(citusScanState->tuplestorestate);
}


/*
 * ReadFileIntoTupleStore parses the records in a COPY-formatted file according
 * according to the given tuple descriptor and stores the records in a tuple
 * store.
 */
void
ReadFileIntoTupleStore(char *fileName, char *copyFormat, TupleDesc tupleDescriptor,
					   Tuplestorestate *tupstore)
{
	CopyState copyState = NULL;

	/*
	 * Trick BeginCopyFrom into using our tuple descriptor by pretending it belongs
	 * to a relation.
	 */
	Relation stubRelation = StubRelation(tupleDescriptor);

	EState *executorState = CreateExecutorState();
	MemoryContext executorTupleContext = GetPerTupleMemoryContext(executorState);
	ExprContext *executorExpressionContext = GetPerTupleExprContext(executorState);

	int columnCount = tupleDescriptor->natts;
	Datum *columnValues = palloc0(columnCount * sizeof(Datum));
	bool *columnNulls = palloc0(columnCount * sizeof(bool));

	DefElem *copyOption = NULL;
	List *copyOptions = NIL;

	int location = -1; /* "unknown" token location */
	copyOption = makeDefElem("format", (Node *) makeString(copyFormat), location);
	copyOptions = lappend(copyOptions, copyOption);

	copyState = BeginCopyFrom(NULL, stubRelation, fileName, false, NULL,
							  NULL, copyOptions);

	while (true)
	{
		MemoryContext oldContext = NULL;
		bool nextRowFound = false;

		ResetPerTupleExprContext(executorState);
		oldContext = MemoryContextSwitchTo(executorTupleContext);

		nextRowFound = NextCopyFrom(copyState, executorExpressionContext,
									columnValues, columnNulls, NULL);
		if (!nextRowFound)
		{
			MemoryContextSwitchTo(oldContext);
			break;
		}

		tuplestore_putvalues(tupstore, tupleDescriptor, columnValues, columnNulls);
		MemoryContextSwitchTo(oldContext);
	}

	EndCopyFrom(copyState);
	pfree(columnValues);
	pfree(columnNulls);
}


/*
 * StubRelation creates a stub Relation from the given tuple descriptor.
 * To be able to use copy.c, we need a Relation descriptor. As there is no
 * relation corresponding to the data loaded from workers, we need to fake one.
 * We just need the bare minimal set of fields accessed by BeginCopyFrom().
 */
static Relation
StubRelation(TupleDesc tupleDescriptor)
{
	Relation stubRelation = palloc0(sizeof(RelationData));
	stubRelation->rd_att = tupleDescriptor;
	stubRelation->rd_rel = palloc0(sizeof(FormData_pg_class));
	stubRelation->rd_rel->relkind = RELKIND_RELATION;

	return stubRelation;
}


/*
 * ExecuteQueryStringIntoDestReceiver plans and executes a query and sends results
 * to the given DestReceiver.
 */
void
ExecuteQueryStringIntoDestReceiver(const char *queryString, ParamListInfo params,
								   DestReceiver *dest)
{
	Query *query = ParseQueryString(queryString);

	ExecuteQueryIntoDestReceiver(query, params, dest);
}


/*
 * ParseQuery parses query string and returns a Query struct.
 */
Query *
ParseQueryString(const char *queryString)
{
	Query *query = NULL;
	RawStmt *rawStmt = (RawStmt *) ParseTreeRawStmt(queryString);
	List *queryTreeList = pg_analyze_and_rewrite(rawStmt, queryString, NULL, 0, NULL);

	if (list_length(queryTreeList) != 1)
	{
		ereport(ERROR, (errmsg("can only execute a single query")));
	}

	query = (Query *) linitial(queryTreeList);

	return query;
}


/*
 * ExecuteQueryIntoDestReceiver plans and executes a query and sends results to the given
 * DestReceiver.
 */
void
ExecuteQueryIntoDestReceiver(Query *query, ParamListInfo params, DestReceiver *dest)
{
	PlannedStmt *queryPlan = NULL;
	int cursorOptions = 0;

	cursorOptions = CURSOR_OPT_PARALLEL_OK;

	/* plan the subquery, this may be another distributed query */
	queryPlan = pg_plan_query(query, cursorOptions, params);

	ExecutePlanIntoDestReceiver(queryPlan, params, dest);
}


/*
 * ExecuteIntoDestReceiver plans and executes a query and sends results to the given
 * DestReceiver.
 */
void
ExecutePlanIntoDestReceiver(PlannedStmt *queryPlan, ParamListInfo params,
							DestReceiver *dest)
{
	Portal portal = NULL;
	int eflags = 0;
	long count = FETCH_ALL;

	/* create a new portal for executing the query */
	portal = CreateNewPortal();

	/* don't display the portal in pg_cursors, it is for internal use only */
	portal->visible = false;

	PortalDefineQuery(portal,
					  NULL,
					  "",
					  "SELECT",
					  list_make1(queryPlan),
					  NULL);

	PortalStart(portal, params, eflags, GetActiveSnapshot());
	PortalRun(portal, count, false, true, dest, dest, NULL);
	PortalDrop(portal, false);
}


/*
 * SetLocalMultiShardModifyModeToSequential simply a C interface for
 * setting the following:
 *      SET LOCAL citus.multi_shard_modify_mode = 'sequential';
 */
void
SetLocalMultiShardModifyModeToSequential()
{
	set_config_option("citus.multi_shard_modify_mode", "sequential",
					  (superuser() ? PGC_SUSET : PGC_USERSET), PGC_S_SESSION,
					  GUC_ACTION_LOCAL, true, 0, false);
}


/*
 * AlterTableConstraintCheck returns if the given query is an ALTER TABLE
 * constraint check query.
 *
 * Postgres uses SPI to execute these queries. To see examples of how these
 * constraint check queries look like, see RI_Initial_Check() and RI_Fkey_check().
 */
static bool
AlterTableConstraintCheck(QueryDesc *queryDesc)
{
	if (!AlterTableInProgress())
	{
		return false;
	}

	/*
	 * These queries are one or more SELECT queries, where postgres checks
	 * their results either for NULL values or existence of a row at all.
	 */
	if (queryDesc->plannedstmt->commandType != CMD_SELECT)
	{
		return false;
	}

	/*
	 * While an ALTER TABLE is in progress, we might do SELECTs on some
	 * catalog tables too. For example, when dropping a column, citus_drop_trigger()
	 * runs some SELECTs on catalog tables. These are not constraint check queries.
	 */
	if (!IsCitusPlan(queryDesc->plannedstmt->planTree))
	{
		return false;
	}

	return true;
}
