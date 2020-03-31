/*-------------------------------------------------------------------------
 *
 * spock_functions.c
 *		spock SQL visible interfaces
 *
 * Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		spock_functions.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/commit_ts.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/xlog.h"

#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"

#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"

#include "executor/spi.h"

#include "funcapi.h"

#include "miscadmin.h"

#include "nodes/makefuncs.h"

#include "pgtime.h"

#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"

#include "replication/origin.h"
#include "replication/reorderbuffer.h"
#include "replication/slot.h"

#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"

#include "tcop/tcopprot.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/json.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "pgstat.h"

#include "spock_dependency.h"
#include "spock_node.h"
#include "spock_executor.h"
#include "spock_queue.h"
#include "spock_relcache.h"
#include "spock_repset.h"
#include "spock_rpc.h"
#include "spock_sync.h"
#include "spock_worker.h"

#include "spock.h"

/* Node management. */
PG_FUNCTION_INFO_V1(spock_create_node);
PG_FUNCTION_INFO_V1(spock_drop_node);
PG_FUNCTION_INFO_V1(spock_alter_node_add_interface);
PG_FUNCTION_INFO_V1(spock_alter_node_drop_interface);

/* Subscription management. */
PG_FUNCTION_INFO_V1(spock_create_subscription);
PG_FUNCTION_INFO_V1(spock_drop_subscription);

PG_FUNCTION_INFO_V1(spock_alter_subscription_interface);

PG_FUNCTION_INFO_V1(spock_alter_subscription_disable);
PG_FUNCTION_INFO_V1(spock_alter_subscription_enable);

PG_FUNCTION_INFO_V1(spock_alter_subscription_add_replication_set);
PG_FUNCTION_INFO_V1(spock_alter_subscription_remove_replication_set);

PG_FUNCTION_INFO_V1(spock_alter_subscription_synchronize);
PG_FUNCTION_INFO_V1(spock_alter_subscription_resynchronize_table);

PG_FUNCTION_INFO_V1(spock_show_subscription_table);
PG_FUNCTION_INFO_V1(spock_show_subscription_status);

PG_FUNCTION_INFO_V1(spock_wait_for_subscription_sync_complete);
PG_FUNCTION_INFO_V1(spock_wait_for_table_sync_complete);

/* Replication set manipulation. */
PG_FUNCTION_INFO_V1(spock_create_replication_set);
PG_FUNCTION_INFO_V1(spock_alter_replication_set);
PG_FUNCTION_INFO_V1(spock_drop_replication_set);
PG_FUNCTION_INFO_V1(spock_replication_set_add_table);
PG_FUNCTION_INFO_V1(spock_replication_set_add_all_tables);
PG_FUNCTION_INFO_V1(spock_replication_set_remove_table);
PG_FUNCTION_INFO_V1(spock_replication_set_add_sequence);
PG_FUNCTION_INFO_V1(spock_replication_set_add_all_sequences);
PG_FUNCTION_INFO_V1(spock_replication_set_remove_sequence);

/* Other manipulation function */
PG_FUNCTION_INFO_V1(spock_synchronize_sequence);

/* DDL */
PG_FUNCTION_INFO_V1(spock_replicate_ddl_command);
PG_FUNCTION_INFO_V1(spock_queue_truncate);
PG_FUNCTION_INFO_V1(spock_truncate_trigger_add);
PG_FUNCTION_INFO_V1(spock_dependency_check_trigger);

/* Internal utils */
PG_FUNCTION_INFO_V1(spock_gen_slot_name);
PG_FUNCTION_INFO_V1(spock_node_info);
PG_FUNCTION_INFO_V1(spock_show_repset_table_info);
PG_FUNCTION_INFO_V1(spock_show_repset_table_info_by_target);
PG_FUNCTION_INFO_V1(spock_table_data_filtered);

/* Information */
PG_FUNCTION_INFO_V1(spock_version);
PG_FUNCTION_INFO_V1(spock_version_num);
PG_FUNCTION_INFO_V1(spock_min_proto_version);
PG_FUNCTION_INFO_V1(spock_max_proto_version);

PG_FUNCTION_INFO_V1(spock_xact_commit_timestamp_origin);

static void gen_slot_name(Name slot_name, char *dbname,
						  const char *provider_name,
						  const char *subscriber_name);

bool in_spock_replicate_ddl_command = false;

static SpockLocalNode *
check_local_node(bool for_update)
{
	SpockLocalNode *node;

	node = get_local_node(for_update, true);
	if (!node)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current database is not configured as spock node"),
				 errhint("create spock node first")));

	return node;
}

/*
 * Create new node
 */
Datum
spock_create_node(PG_FUNCTION_ARGS)
{
	char			   *node_name = NameStr(*PG_GETARG_NAME(0));
	char			   *node_dsn = text_to_cstring(PG_GETARG_TEXT_PP(1));
	SpockNode		node;
	SpockInterface	nodeif;
	SpockRepSet		repset;

	node.id = InvalidOid;
	node.name = node_name;
	create_node(&node);

	nodeif.id = InvalidOid;
	nodeif.name = node.name;
	nodeif.nodeid = node.id;
	nodeif.dsn = node_dsn;
	create_node_interface(&nodeif);

	/* Create predefined repsets. */
	repset.id = InvalidOid;
	repset.nodeid = node.id;
	repset.name = DEFAULT_REPSET_NAME;
	repset.replicate_insert = true;
	repset.replicate_update = true;
	repset.replicate_delete = true;
	repset.replicate_truncate = true;
	create_replication_set(&repset);

	repset.id = InvalidOid;
	repset.nodeid = node.id;
	repset.name = DEFAULT_INSONLY_REPSET_NAME;
	repset.replicate_insert = true;
	repset.replicate_update = false;
	repset.replicate_delete = false;
	repset.replicate_truncate = true;
	create_replication_set(&repset);

	repset.id = InvalidOid;
	repset.nodeid = node.id;
	repset.name = DDL_SQL_REPSET_NAME;
	repset.replicate_insert = true;
	repset.replicate_update = false;
	repset.replicate_delete = false;
	repset.replicate_truncate = false;
	create_replication_set(&repset);

	create_local_node(node.id, nodeif.id);

	PG_RETURN_OID(node.id);
}

/*
 * Drop the named node.
 *
 * TODO: support cascade (drop subscribers)
 */
Datum
spock_drop_node(PG_FUNCTION_ARGS)
{
	char	   *node_name = NameStr(*PG_GETARG_NAME(0));
	bool		ifexists = PG_GETARG_BOOL(1);
	SpockNode  *node;

	node = get_node_by_name(node_name, ifexists);

	if (node != NULL)
	{
		SpockLocalNode *local_node;
		List			   *osubs;
		List			   *tsubs;

		osubs = get_node_subscriptions(node->id, true);
		tsubs = get_node_subscriptions(node->id, false);
		if (list_length(osubs) != 0 || list_length(tsubs) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot drop node \"%s\" because it still has subscriptions associated with it", node_name),
					 errhint("drop the subscriptions first")));

		/* If the node is local node, drop the record as well. */
		local_node = get_local_node(true, true);
		if (local_node && local_node->node->id == node->id)
		{
			int		res;

			/*
			 * Also drop all the slots associated with the node.
			 *
			 * We do this via SPI mainly because ReplicationSlotCtl is not
			 * accessible on Windows.
			 */
			SPI_connect();
			PG_TRY();
			{
				res = SPI_execute("SELECT pg_catalog.pg_drop_replication_slot(slot_name)"
								  "  FROM pg_catalog.pg_replication_slots"
								  " WHERE (plugin = 'spock_output' OR plugin = 'spock')"
								  "   AND database = current_database()"
								  "   AND slot_name ~ 'spk_.*'",
								  false, 0);
			}
			PG_CATCH();
			{
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot drop node \"%s\" because one or more replication slots for the node are still active",
								node_name),
						 errhint("drop the subscriptions connected to the node first")));
			}
			PG_END_TRY();

			if (res != SPI_OK_SELECT)
				elog(ERROR, "SPI query failed: %d", res);

			SPI_finish();

			/* And drop the local node association as well. */
			drop_local_node();
		}

		/* Drop all the interfaces. */
		drop_node_interfaces(node->id);

		/* Drop replication sets associated with the node. */
		drop_node_replication_sets(node->id);

		/* Drop the node itself. */
		drop_node(node->id);
	}

	PG_RETURN_BOOL(node != NULL);
}

/*
 * Add interface to a node.
 */
Datum
spock_alter_node_add_interface(PG_FUNCTION_ARGS)
{
	char	   *node_name = NameStr(*PG_GETARG_NAME(0));
	char	   *if_name = NameStr(*PG_GETARG_NAME(1));
	char	   *if_dsn = text_to_cstring(PG_GETARG_TEXT_PP(2));
	SpockNode	   *node;
	SpockInterface *oldif,
						newif;

	node = get_node_by_name(node_name, false);
	if (node == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("node \"%s\" not found", node_name)));

	oldif = get_node_interface_by_name(node->id, if_name, true);
	if (oldif != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("node \"%s\" already has interface named \"%s\"",
				 node_name, if_name)));

	newif.id = InvalidOid;
	newif.name = if_name;
	newif.nodeid = node->id;
	newif.dsn = if_dsn;
	create_node_interface(&newif);

	PG_RETURN_OID(newif.id);
}

/*
 * Drop interface from a node.
 */
Datum
spock_alter_node_drop_interface(PG_FUNCTION_ARGS)
{
	char	   *node_name = NameStr(*PG_GETARG_NAME(0));
	char	   *if_name = NameStr(*PG_GETARG_NAME(1));
	SpockNode	   *node;
	SpockInterface *oldif;
	List		   *other_subs;
	ListCell	   *lc;

	node = get_node_by_name(node_name, false);
	if (node == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("node \"%s\" not found", node_name)));

	oldif = get_node_interface_by_name(node->id, if_name, true);
	if (oldif == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interface \"%s\" for node node \"%s\" not found",
				 if_name, node_name)));

	other_subs = get_node_subscriptions(node->id, true);
	foreach (lc, other_subs)
	{
		SpockSubscription  *sub = (SpockSubscription *) lfirst(lc);
		if (oldif->id == sub->origin_if->id)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot drop interface \"%s\" for node \"%s\" because subscription \"%s\" is using it",
							oldif->name, node->name, sub->name),
					 errhint("change the subscription interface first")));
        }

	drop_node_interface(oldif->id);

	PG_RETURN_BOOL(true);
}


/*
 * Connect two existing nodes.
 */
Datum
spock_create_subscription(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	char				   *provider_dsn = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ArrayType			   *rep_set_names = PG_GETARG_ARRAYTYPE_P(2);
	char					*sync_structure = text_to_cstring(PG_GETARG_TEXT_PP(3));
	bool					sync_data = PG_GETARG_BOOL(4);
	ArrayType			   *forward_origin_names = PG_GETARG_ARRAYTYPE_P(5);
	Interval			   *apply_delay = PG_GETARG_INTERVAL_P(6);
	bool					force_text_transfer = PG_GETARG_BOOL(7);
	PGconn				   *conn;
	SpockSubscription	sub;
	SpockSyncStatus		sync;
	SpockNode			origin;
	SpockNode		   *existing_origin;
	SpockInterface		originif;
	SpockLocalNode     *localnode;
	SpockInterface		targetif;
	List				   *replication_sets;
	List				   *other_subs;
	ListCell			   *lc;
	NameData				slot_name;

	/* Check that this is actually a node. */
	localnode = get_local_node(true, false);

	/* Now, fetch info about remote node. */
	conn = spock_connect(provider_dsn, sub_name, "create");
	spock_remote_node_info(conn, &origin.id, &origin.name, NULL, NULL, NULL);
	PQfinish(conn);

	/* Check that we can connect remotely also in replication mode. */
	conn = spock_connect_replica(provider_dsn, sub_name, "create");
	PQfinish(conn);

	/* Check that local connection works. */
	conn = spock_connect(localnode->node_if->dsn, sub_name, "create");
	PQfinish(conn);

	/*
	 * Check for existing local representation of remote node and interface
	 * and lock it if it already exists.
	 */
	existing_origin = get_node_by_name(origin.name, true);

	/*
	 * If not found, crate local representation of remote node and interface.
	 */
	if (!existing_origin)
	{
		create_node(&origin);

		originif.id = InvalidOid;
		originif.name = origin.name;
		originif.nodeid = origin.id;
		originif.dsn = provider_dsn;
		create_node_interface(&originif);
	}
	else
	{
		SpockInterface *existingif;

		existingif = get_node_interface_by_name(origin.id, origin.name, false);
		if (strcmp(existingif->dsn, provider_dsn) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("dsn \"%s\" points to existing node \"%s\" with different dsn \"%s\"",
					 provider_dsn, origin.name, existingif->dsn)));

		memcpy(&originif, existingif, sizeof(SpockInterface));
	}

	/*
	 * Check for overlapping replication sets.
	 *
	 * Note that we can't use exclusion constraints as we use the
	 * subscriptions table in same manner as system catalog.
	 */
	replication_sets = textarray_to_list(rep_set_names);
	other_subs = get_node_subscriptions(originif.nodeid, true);
	foreach (lc, other_subs)
	{
		SpockSubscription  *esub = (SpockSubscription *) lfirst(lc);
		ListCell			   *esetcell;

		foreach (esetcell, esub->replication_sets)
		{
			char	   *existingset = lfirst(esetcell);
			ListCell   *nsetcell;

			foreach (nsetcell, replication_sets)
			{
				char	   *newset = lfirst(nsetcell);

				if (strcmp(newset, existingset) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("existing subscription \"%s\" to node "
									"\"%s\" already subscribes to replication "
									"set \"%s\"", esub->name, origin.name,
									newset)));
			}
		}
	}

	/*
	 * Create the subscription.
	 *
	 * Note for now we don't care much about the target interface so we fake
	 * it here to be invalid.
	 */
	targetif.id = localnode->node_if->id;
	targetif.nodeid = localnode->node->id;
	sub.id = InvalidOid;
	sub.name = sub_name;
	sub.origin_if = &originif;
	sub.target_if = &targetif;
	sub.replication_sets = replication_sets;
	sub.forward_origins = textarray_to_list(forward_origin_names);
	sub.enabled = true;
	gen_slot_name(&slot_name, get_database_name(MyDatabaseId),
				  origin.name, sub_name);
	sub.slot_name = pstrdup(NameStr(slot_name));
	sub.apply_delay = apply_delay;
	sub.force_text_transfer = force_text_transfer;

	create_subscription(&sub);

	/* Create synchronization status for the subscription. */
	memset(&sync, 0, sizeof(SpockSyncStatus));

	if (SyncStructureAll(sync_structure) && sync_data)
		sync.kind = SYNC_KIND_FULL;
	else if (SyncStructureRelOnly(sync_structure) && sync_data)
		sync.kind = SYNC_KIND_FULL_REL;
	else if (SyncStructureAll(sync_structure))
		sync.kind = SYNC_KIND_STRUCTURE;
	else if (SyncStructureRelOnly(sync_structure))
		sync.kind = SYNC_KIND_STRUCTURE_REL;
	else if (sync_data)
		sync.kind = SYNC_KIND_DATA;
	else
		sync.kind = SYNC_KIND_INIT;

	sync.subid = sub.id;
	sync.status = SYNC_STATUS_INIT;
	create_local_sync_status(&sync);

	PG_RETURN_OID(sub.id);
}

/*
 * Remove subscribption.
 */
Datum
spock_drop_subscription(PG_FUNCTION_ARGS)
{
	char	   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool		ifexists = PG_GETARG_BOOL(1);
	SpockSubscription  *sub;

	sub = get_subscription_by_name(sub_name, ifexists);

	if (sub != NULL)
	{
		SpockWorker	   *apply;
		List			   *other_subs;
		SpockLocalNode *node;
		RepOriginId			originid;

		node = get_local_node(true, false);

		/* First drop the status. */
		drop_subscription_sync_status(sub->id);

		/* Drop the actual subscription. */
		drop_subscription(sub->id);

		/*
		 * The rest is different depending on if we are doing this on provider
		 * or subscriber.
		 *
		 * For now on provider we just exist (there should be no records
		 * of subscribers on their provider node).
		 */
		if (sub->origin->id == node->node->id)
			PG_RETURN_BOOL(sub != NULL);

		/*
		 * If the provider node record existed only for the dropped,
		 * subscription, it should be dropped as well.
		 */
		other_subs = get_node_subscriptions(sub->origin->id, true);
		if (list_length(other_subs) == 0)
		{
			drop_node_interfaces(sub->origin->id);
			drop_node(sub->origin->id);
		}

		/* Kill the apply to unlock the resources. */
		LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);
		apply = spock_apply_find(MyDatabaseId, sub->id);
		spock_worker_kill(apply);
		LWLockRelease(SpockCtx->lock);

		/* Wait for the apply to die. */
		for (;;)
		{
			int rc;

			LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);
			apply = spock_apply_find(MyDatabaseId, sub->id);
			if (!spock_worker_running(apply))
			{
				LWLockRelease(SpockCtx->lock);
				break;
			}
			LWLockRelease(SpockCtx->lock);

			CHECK_FOR_INTERRUPTS();

			rc = WaitLatch(&MyProc->procLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 1000L);

			if (rc & WL_POSTMASTER_DEATH)
				proc_exit(1);

			ResetLatch(&MyProc->procLatch);
		}

		/*
		 * Drop the slot on remote side.
		 *
		 * Note, we can't fail here since we can't assume that the remote node
		 * is still reachable or even alive.
		 */
		PG_TRY();
		{
			PGconn *origin_conn = spock_connect(sub->origin_if->dsn,
													sub->name, "cleanup");
			spock_drop_remote_slot(origin_conn, sub->slot_name);
			PQfinish(origin_conn);
		}
		PG_CATCH();
			elog(WARNING, "could not drop slot \"%s\" on provider, you will probably have to drop it manually",
				 sub->slot_name);
		PG_END_TRY();

		/* Drop the origin tracking locally. */
		originid = replorigin_by_name(sub->slot_name, true);
		if (originid != InvalidRepOriginId)
			spk_replorigin_drop(originid);
	}

	PG_RETURN_BOOL(sub != NULL);
}

/*
 * Disable subscription.
 */
Datum
spock_alter_subscription_disable(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool					immediate = PG_GETARG_BOOL(1);
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);

	/* XXX: Only used for locking purposes. */
	(void) get_local_node(true, false);

	sub->enabled = false;

	alter_subscription(sub);

	if (immediate)
	{
		SpockWorker		   *apply;

		if ((IsTransactionBlock() || IsSubTransaction()))
			ereport(ERROR,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
					 errmsg("alter_subscription_disable with immediate = true "
							"cannot be run inside a transaction block")));

		LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);
		apply = spock_apply_find(MyDatabaseId, sub->id);
		spock_worker_kill(apply);
		LWLockRelease(SpockCtx->lock);
	}

	PG_RETURN_BOOL(true);
}

/*
 * Enable subscription.
 */
Datum
spock_alter_subscription_enable(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool					immediate = PG_GETARG_BOOL(1);
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);

	/* XXX: Only used for locking purposes. */
	(void) get_local_node(true, false);

	sub->enabled = true;

	alter_subscription(sub);

	/*
	 * There is nothing more to immediate here than running it outside of
	 * transaction.
	 */
	if (immediate && (IsTransactionBlock() || IsSubTransaction()))
	{
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("alter_subscription_enable with immediate = true "
						"cannot be run inside a transaction block")));
	}

	PG_RETURN_BOOL(true);
}

/*
 * Switch interface the subscription is using.
 */
Datum
spock_alter_subscription_interface(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	char				   *if_name = NameStr(*PG_GETARG_NAME(1));
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);
	SpockInterface	   *new_if;

	/* XXX: Only used for locking purposes. */
	(void) get_local_node(true, false);

	new_if = get_node_interface_by_name(sub->origin->id, if_name, false);

	if (new_if->id == sub->origin_if->id)
		PG_RETURN_BOOL(false);

	sub->origin_if = new_if;
	alter_subscription(sub);

	PG_RETURN_BOOL(true);
}

/*
 * Add replication set to subscription.
 */
Datum
spock_alter_subscription_add_replication_set(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	char				   *repset_name = NameStr(*PG_GETARG_NAME(1));
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);
	ListCell			   *lc;

	foreach (lc, sub->replication_sets)
	{
		char	   *rs = (char *) lfirst(lc);

		if (strcmp(rs, repset_name) == 0)
			PG_RETURN_BOOL(false);
	}

	sub->replication_sets = lappend(sub->replication_sets, repset_name);
	alter_subscription(sub);

	PG_RETURN_BOOL(true);
}

/*
 * Remove replication set to subscription.
 */
Datum
spock_alter_subscription_remove_replication_set(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	char				   *repset_name = NameStr(*PG_GETARG_NAME(1));
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);
	ListCell			   *lc,
						   *next,
						   *prev;

	prev = NULL;
	for (lc = list_head(sub->replication_sets); lc; lc = next)
	{
		char	   *rs = (char *) lfirst(lc);

		/* We might delete the cell so advance it now. */
		next = lnext(lc);

		if (strcmp(rs, repset_name) == 0)
		{
			sub->replication_sets = list_delete_cell(sub->replication_sets,
													 lc, prev);
			alter_subscription(sub);

			PG_RETURN_BOOL(true);
		}

		prev = lc;
	}

	PG_RETURN_BOOL(false);
}

/*
 * Synchronize all the missing tables.
 */
Datum
spock_alter_subscription_synchronize(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool					truncate = PG_GETARG_BOOL(1);
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);
	PGconn				   *conn;
	List				   *remote_tables;
	List				   *local_tables;
	ListCell			   *lc;

	/* Read table list from provider. */
	conn = spock_connect(sub->origin_if->dsn, sub_name, "sync");
	remote_tables = pg_logical_get_remote_repset_tables(conn, sub->replication_sets);
	PQfinish(conn);

	local_tables = get_subscription_tables(sub->id);

	/* Compare with sync status on subscription. And add missing ones. */
	foreach (lc, remote_tables)
	{
		SpockRemoteRel	   *remoterel = lfirst(lc);
		SpockSyncStatus	   *oldsync = NULL;
		ListCell			   *prev;
		ListCell			   *next;
		ListCell			   *llc;

		prev = NULL;
		for (llc = list_head(local_tables); llc; llc = next)
		{
			SpockSyncStatus *tablesync = (SpockSyncStatus *) lfirst(llc);

			/* We might delete the cell so advance it now. */
			next = lnext(llc);

			if (namestrcmp(&tablesync->nspname, remoterel->nsptarget) == 0 &&
				namestrcmp(&tablesync->relname, remoterel->reltarget) == 0)
			{
				oldsync = tablesync;
				local_tables = list_delete_cell(local_tables, llc, prev);
				break;
			}
			else
				prev = llc;
		}

		if (!oldsync)
		{
			SpockSyncStatus	   newsync;

			memset(&newsync, 0, sizeof(SpockSyncStatus));
			newsync.kind = SYNC_KIND_DATA;
			newsync.subid = sub->id;
			namestrcpy(&newsync.nspname, remoterel->nsptarget);
			namestrcpy(&newsync.relname, remoterel->reltarget);
			newsync.status = SYNC_STATUS_INIT;
			create_local_sync_status(&newsync);

			if (truncate)
				truncate_table(remoterel->nsptarget, remoterel->reltarget);
		}
	}

	/*
	 * Any leftover local tables should not be replicated, remove the status
	 * for them.
	 */
	foreach (lc, local_tables)
	{
		SpockSyncStatus *tablesync = (SpockSyncStatus *) lfirst(lc);

		drop_table_sync_status_for_sub(tablesync->subid,
									   NameStr(tablesync->nspname),
									   NameStr(tablesync->relname));
	}

	/* Tell apply to re-read sync statuses. */
	spock_subscription_changed(sub->id, false);

	PG_RETURN_BOOL(true);
}

/*
 * Resynchronize one existing table.
 */
Datum
spock_alter_subscription_resynchronize_table(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	Oid						reloid = PG_GETARG_OID(1);
	bool					truncate = PG_GETARG_BOOL(2);
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);
	SpockSyncStatus	   *oldsync;
	Relation				rel;
	char				   *nspname,
						   *relname;

	rel = table_open(reloid, AccessShareLock);

	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);

	/* Reset sync status of the table. */
	oldsync = get_table_sync_status(sub->id, nspname, relname, true);
	if (oldsync)
	{
		if (oldsync->status != SYNC_STATUS_READY &&
			oldsync->status != SYNC_STATUS_SYNCDONE &&
			oldsync->status != SYNC_STATUS_NONE)
			elog(ERROR, "table %s.%s is already being synchronized",
				 nspname, relname);

		set_table_sync_status(sub->id, nspname, relname, SYNC_STATUS_INIT,
							  InvalidXLogRecPtr);
	}
	else
	{
		SpockSyncStatus	   newsync;

		memset(&newsync, 0, sizeof(SpockSyncStatus));
		newsync.kind = SYNC_KIND_DATA;
		newsync.subid = sub->id;
		namestrcpy(&newsync.nspname, nspname);
		namestrcpy(&newsync.relname, relname);
		newsync.status = SYNC_STATUS_INIT;
		create_local_sync_status(&newsync);
	}

	table_close(rel, NoLock);

	if (truncate)
		truncate_table(nspname, relname);

	/* Tell apply to re-read sync statuses. */
	spock_subscription_changed(sub->id, false);

	PG_RETURN_BOOL(true);
}

/*
 * Synchronize one sequence.
 */
Datum
spock_synchronize_sequence(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);

	/* Check that this is actually a node. */
	(void) get_local_node(true, false);

	synchronize_sequence(reloid);

	PG_RETURN_BOOL(true);
}

static char *
sync_status_to_string(char status)
{
	switch (status)
	{
		case SYNC_STATUS_INIT:
			return "sync_init";
		case SYNC_STATUS_STRUCTURE:
			return "sync_structure";
		case SYNC_STATUS_DATA:
			return "sync_data";
		case SYNC_STATUS_CONSTAINTS:
			return "sync_constraints";
		case SYNC_STATUS_SYNCWAIT:
			return "sync_waiting";
		case SYNC_STATUS_CATCHUP:
			return "catchup";
		case SYNC_STATUS_SYNCDONE:
			return "synchronized";
		case SYNC_STATUS_READY:
			return "replicating";
		default:
			return "unknown";
	}
}

/*
 * Show info about one table.
 */
Datum
spock_show_subscription_table(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	Oid						reloid = PG_GETARG_OID(1);
	SpockSubscription  *sub = get_subscription_by_name(sub_name, false);
	char				   *nspname;
	char				   *relname;
	SpockSyncStatus	   *sync;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Datum		values[3];
	bool		nulls[3];

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	nspname = get_namespace_name(get_rel_namespace(reloid));
	relname = get_rel_name(reloid);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	values[0] = CStringGetTextDatum(nspname);
	values[1] = CStringGetTextDatum(relname);

	/* Reset sync status of the table. */
	sync = get_table_sync_status(sub->id, nspname, relname, true);
	if (sync)
		values[2] = CStringGetTextDatum(sync_status_to_string(sync->status));
	else
		values[2] = CStringGetTextDatum("unknown");

	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/*
 * Show info about subscribtion.
 */
Datum
spock_show_subscription_status(PG_FUNCTION_ARGS)
{
	List			   *subscriptions;
	ListCell		   *lc;
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	SpockLocalNode *node;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	node = check_local_node(false);

	if (PG_ARGISNULL(0))
	{
		subscriptions = get_node_subscriptions(node->node->id, false);
	}
	else
	{
		SpockSubscription  *sub;
		sub = get_subscription_by_name(NameStr(*PG_GETARG_NAME(0)), false);
		subscriptions = list_make1(sub);
	}

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	foreach (lc, subscriptions)
	{
		SpockSubscription  *sub = lfirst(lc);
		SpockWorker		   *apply;
		Datum	values[7];
		bool	nulls[7];
		char   *status;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		LWLockAcquire(SpockCtx->lock, LW_EXCLUSIVE);
		apply = spock_apply_find(MyDatabaseId, sub->id);
		if (spock_worker_running(apply))
		{
			SpockSyncStatus	   *sync;
			sync = get_subscription_sync_status(sub->id, true);

			if (!sync)
				status = "unknown";
			else if (sync->status == SYNC_STATUS_READY)
				status = "replicating";
			else
				status = "initializing";
		}
		else if (!sub->enabled)
			status = "disabled";
		else
			status = "down";
		LWLockRelease(SpockCtx->lock);

		values[0] = CStringGetTextDatum(sub->name);
		values[1] = CStringGetTextDatum(status);
		values[2] = CStringGetTextDatum(sub->origin->name);
		values[3] = CStringGetTextDatum(sub->origin_if->dsn);
		values[4] = CStringGetTextDatum(sub->slot_name);
		if (sub->replication_sets)
			values[5] =
				PointerGetDatum(strlist_to_textarray(sub->replication_sets));
		else
			nulls[5] = true;
		if (sub->forward_origins)
			values[6] =
				PointerGetDatum(strlist_to_textarray(sub->forward_origins));
		else
			nulls[6] = true;

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/*
 * Create new replication set.
 */
Datum
spock_create_replication_set(PG_FUNCTION_ARGS)
{
	SpockRepSet		repset;
	SpockLocalNode *node;

	node = check_local_node(true);

	repset.id = InvalidOid;

	repset.nodeid = node->node->id;
	repset.name = NameStr(*PG_GETARG_NAME(0));

	repset.replicate_insert = PG_GETARG_BOOL(1);
	repset.replicate_update = PG_GETARG_BOOL(2);
	repset.replicate_delete = PG_GETARG_BOOL(3);
	repset.replicate_truncate = PG_GETARG_BOOL(4);

	create_replication_set(&repset);

	PG_RETURN_OID(repset.id);
}

/*
 * Alter existing replication set.
 */
Datum
spock_alter_replication_set(PG_FUNCTION_ARGS)
{
	SpockRepSet	   *repset;
	SpockLocalNode *node;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("set_name cannot be NULL")));

	node = check_local_node(true);

	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*PG_GETARG_NAME(0)), false);

	if (!PG_ARGISNULL(1))
		repset->replicate_insert = PG_GETARG_BOOL(1);
	if (!PG_ARGISNULL(2))
		repset->replicate_update = PG_GETARG_BOOL(2);
	if (!PG_ARGISNULL(3))
		repset->replicate_delete = PG_GETARG_BOOL(3);
	if (!PG_ARGISNULL(4))
		repset->replicate_truncate = PG_GETARG_BOOL(4);

	alter_replication_set(repset);

	PG_RETURN_OID(repset->id);
}

/*
 * Drop existing replication set.
 */
Datum
spock_drop_replication_set(PG_FUNCTION_ARGS)
{
	char	   *set_name = NameStr(*PG_GETARG_NAME(0));
	bool		ifexists = PG_GETARG_BOOL(1);
	SpockRepSet    *repset;
	SpockLocalNode *node;

	node = check_local_node(true);

	repset = get_replication_set_by_name(node->node->id, set_name, ifexists);

	if (repset != NULL)
		drop_replication_set(repset->id);

	PG_RETURN_BOOL(repset != NULL);
}

/*
 * error context callback for parse failure during spock_replication_set_add_table()
 */
static void
add_table_parser_error_callback(void *arg)
{
	const char *row_filter_str = (const char *) arg;

	errcontext("invalid row_filter expression \"%s\"", row_filter_str);

	/*
	 * Currently we just suppress any syntax error position report, rather
	 * than transforming to an "internal query" error.  It's unlikely that a
	 * type name is complex enough to need positioning.
	 */
	errposition(0);
}

static Node *
parse_row_filter(Relation rel, char *row_filter_str)
{
	Node	   *row_filter = NULL;
	List	   *raw_parsetree_list;
	SelectStmt *stmt;
	ResTarget  *restarget;
	ParseState *pstate;
	char	   *nspname;
	char	   *relname;
	RangeTblEntry *rte;
	StringInfoData buf;
	ErrorContextCallback myerrcontext;

	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);

	/*
	 * Build fake query which includes the expression so that we can
	 * pass it to the parser.
	 */
	initStringInfo(&buf);
	appendStringInfo(&buf, "SELECT %s FROM %s", row_filter_str,
					 quote_qualified_identifier(nspname, relname));

	/* Parse it, providing proper error context. */
	myerrcontext.callback = add_table_parser_error_callback;
	myerrcontext.arg = (void *) row_filter_str;
	myerrcontext.previous = error_context_stack;
	error_context_stack = &myerrcontext;

	raw_parsetree_list = pg_parse_query(buf.data);

	error_context_stack = myerrcontext.previous;

	/* Validate the output from the parser. */
	if (list_length(raw_parsetree_list) != 1)
		goto fail;
#if PG_VERSION_NUM >= 100000
	stmt = (SelectStmt *) linitial_node(RawStmt, raw_parsetree_list)->stmt;
#else
	stmt = (SelectStmt *) linitial(raw_parsetree_list);
#endif
	if (stmt == NULL ||
		!IsA(stmt, SelectStmt) ||
		stmt->distinctClause != NIL ||
		stmt->intoClause != NULL ||
		stmt->whereClause != NULL ||
		stmt->groupClause != NIL ||
		stmt->havingClause != NULL ||
		stmt->windowClause != NIL ||
		stmt->valuesLists != NIL ||
		stmt->sortClause != NIL ||
		stmt->limitOffset != NULL ||
		stmt->limitCount != NULL ||
		stmt->lockingClause != NIL ||
		stmt->withClause != NULL ||
		stmt->op != SETOP_NONE)
		goto fail;
	if (list_length(stmt->targetList) != 1)
		goto fail;
	restarget = (ResTarget *) linitial(stmt->targetList);
	if (restarget == NULL ||
		!IsA(restarget, ResTarget) ||
		restarget->name != NULL ||
		restarget->indirection != NIL ||
		restarget->val == NULL)
		goto fail;

	row_filter = restarget->val;

	/*
	 * Create a dummy ParseState and insert the target relation as its sole
	 * rangetable entry.  We need a ParseState for transformExpr.
	 */
	pstate = make_parsestate(NULL);
	rte = addRangeTableEntryForRelation(pstate,
										rel,
#if PG_VERSION_NUM >= 120000
										AccessShareLock,
#endif
										NULL,
										false,
										true);
	addRTEtoQuery(pstate, rte, true, true, true);
	/*
	 * Transform the expression and check it follows limits of row_filter
	 * which are same as those of CHECK constraint so we can use the builtin
	 * checks for that.
	 *
	 * TODO: make the errors look more informative (currently they will
	 * complain about CHECK constraint. (Possibly add context?)
	 */
	row_filter = transformExpr(pstate, row_filter, EXPR_KIND_CHECK_CONSTRAINT);
	row_filter = coerce_to_boolean(pstate, row_filter, "row_filter");
	assign_expr_collations(pstate, row_filter);
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("only table \"%s\" can be referenced in row_filter",
						relname)));
	pfree(buf.data);

	return row_filter;

fail:
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("invalid row_filter expression \"%s\"", row_filter_str)));
	return NULL;	/* keep compiler quiet */
}

/*
 * Add replication set / table mapping.
 */
Datum
spock_replication_set_add_table(PG_FUNCTION_ARGS)
{
	Name				repset_name;
	Oid					reloid;
	bool				synchronize;
	Node			   *row_filter = NULL;
	List			   *att_list = NIL;
	SpockRepSet    *repset;
	Relation			rel;
	TupleDesc			tupDesc;
	SpockLocalNode *node;
	char			   *nspname;
	char			   *relname;
	char			   *nsptarget;
	char			   *reltarget;
	StringInfoData		json;

	/* Proccess for required parameters. */
	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("set_name cannot be NULL")));
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));
	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("synchronize_data cannot be NULL")));

	repset_name = PG_GETARG_NAME(0);
	reloid = PG_GETARG_OID(1);
	synchronize = PG_GETARG_BOOL(2);

	/* standard check for node. */
	node = check_local_node(true);

	/* Find the replication set. */
	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*repset_name), false);

	/*
	 * Make sure the relation exists (lock mode has to be the same one as
	 * in replication_set_add_relation).
	 */
	rel = table_open(reloid, ShareRowExclusiveLock);
	tupDesc = RelationGetDescr(rel);

	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);

	/* Proccess att_list. */
	if (!PG_ARGISNULL(3))
	{
		ArrayType  *att_names = PG_GETARG_ARRAYTYPE_P(3);
		ListCell   *lc;
		Bitmapset  *idattrs;

		/* fetch bitmap of REPLICATION IDENTITY attributes */
		idattrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY);

		att_list = textarray_to_list(att_names);
		foreach (lc, att_list)
		{
			char   *attname = (char *) lfirst(lc);
			int		attnum = get_att_num_by_name(tupDesc, attname);

			if (attnum < 0)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("table %s does not have column %s",
								quote_qualified_identifier(nspname, relname),
								attname)));

			idattrs = bms_del_member(idattrs,
								attnum - FirstLowInvalidHeapAttributeNumber);
		}

		if (!bms_is_empty(idattrs))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("REPLICA IDENTITY columns must be replicated")));
	}

	/* Proccess row_filter if any. */
	if (!PG_ARGISNULL(4))
	{
		row_filter = parse_row_filter(rel,
									  text_to_cstring(PG_GETARG_TEXT_PP(4)));
	}

	if (!PG_ARGISNULL(5))
	  nsptarget = NameStr(*PG_GETARG_NAME(5));
	else
	  nsptarget = pstrdup(nspname);
	if (!PG_ARGISNULL(6))
	  reltarget = NameStr(*PG_GETARG_NAME(6));
	else
	  reltarget = pstrdup(relname);

	replication_set_add_table(repset->id, reloid, att_list, row_filter,
							  nsptarget, reltarget);

	if (synchronize)
	{
		/* It's easier to construct json manually than via Jsonb API... */
		initStringInfo(&json);
		appendStringInfo(&json, "{\"schema_name\": ");
		escape_json(&json, nsptarget);
		appendStringInfo(&json, ",\"table_name\": ");
		escape_json(&json, reltarget);
		appendStringInfo(&json, "}");
		/* Queue the synchronize request for replication. */
		queue_message(list_make1(repset->name), GetUserId(),
					  QUEUE_COMMAND_TYPE_TABLESYNC, json.data);
	}

	/* Cleanup. */
	table_close(rel, ShareRowExclusiveLock);

	PG_RETURN_BOOL(true);
}

/*
 * Add replication set / sequence mapping.
 */
Datum
spock_replication_set_add_sequence(PG_FUNCTION_ARGS)
{
	Name				repset_name = PG_GETARG_NAME(0);
	Oid					reloid = PG_GETARG_OID(1);
	bool				synchronize = PG_GETARG_BOOL(2);
	SpockRepSet    *repset;
	Relation			rel;
	SpockLocalNode *node;
	char			   *nspname;
	char			   *relname;
	char			   *nsptarget;
	char			   *reltarget;
	StringInfoData		json;

	node = check_local_node(true);

	/* Find the replication set. */
	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*repset_name), false);

	/*
	 * Make sure the relation exists (lock mode has to be the same one as
	 * in replication_set_add_relation).
	 */
	rel = table_open(reloid, ShareRowExclusiveLock);

	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);
	if (!PG_ARGISNULL(3))
	    nsptarget = NameStr(*PG_GETARG_NAME(3));
	else
	  nsptarget = pstrdup(nspname);
	if (!PG_ARGISNULL(4))
		reltarget = NameStr(*PG_GETARG_NAME(4));
	else
	  reltarget = pstrdup(relname);

	replication_set_add_seq(repset->id, reloid, nsptarget, reltarget);

	if (synchronize)
	{
		/* It's easier to construct json manually than via Jsonb API... */
		initStringInfo(&json);
		appendStringInfo(&json, "{\"schema_name\": ");
		escape_json(&json, nsptarget);
		appendStringInfo(&json, ",\"sequence_name\": ");
		escape_json(&json, reltarget);
        appendStringInfo(&json, ",\"last_value\": \""INT64_FORMAT"\"",
								 sequence_get_last_value(reloid));
		appendStringInfo(&json, "}");

		/* Add sequence to the queue. */
		queue_message(list_make1(repset->name), GetUserId(),
					  QUEUE_COMMAND_TYPE_SEQUENCE, json.data);
	}

	/* Cleanup. */
	table_close(rel, NoLock);

	PG_RETURN_BOOL(true);}

/*
 * Common function for adding replication set / relation mapping based on
 * schemas.
 */
static Datum
spock_replication_set_add_all_relations(Name repset_name,
											ArrayType *nsp_names,
											bool synchronize, char relkind)
{
	SpockRepSet    *repset;
	Relation			rel;
	SpockLocalNode *node;
	ListCell		   *lc;
	List			   *existing_relations = NIL;

	node = check_local_node(true);

	/* Find the replication set. */
	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*repset_name), false);

	existing_relations = replication_set_get_tables(repset->id);
	existing_relations = list_concat_unique_oid(existing_relations,
												replication_set_get_seqs(repset->id));

	rel = table_open(RelationRelationId, RowExclusiveLock);

	foreach (lc, textarray_to_list(nsp_names))
	{
		char	   *nspname = lfirst(lc);
		Oid			nspoid = LookupExplicitNamespace(nspname, false);
		ScanKeyData skey[1];
		SysScanDesc sysscan;
		HeapTuple	tuple;

		ScanKeyInit(&skey[0],
					Anum_pg_class_relnamespace,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(nspoid));

		sysscan = systable_beginscan(rel, ClassNameNspIndexId, true,
									 NULL, 1, skey);

		while (HeapTupleIsValid(tuple = systable_getnext(sysscan)))
		{
			Oid				reloid = HeapTupleGetOid(tuple);
			Form_pg_class	reltup = (Form_pg_class) GETSTRUCT(tuple);

			/*
			 * Only add logged relations which are not system relations
			 * (catalog, toast).
			 */
			if (reltup->relkind != relkind ||
				reltup->relpersistence != RELPERSISTENCE_PERMANENT ||
				IsSystemClass(reloid, reltup))
				continue;

			if (!list_member_oid(existing_relations, reloid))
			{
				if (relkind == RELKIND_RELATION)
					replication_set_add_table(repset->id, reloid, NIL, NULL,
											  NULL, NULL);
				else
					replication_set_add_seq(repset->id, reloid,
											NULL, NULL);

				if (synchronize)
				{
					char			   *relname;
					StringInfoData		json;
					char				cmdtype;

					relname = get_rel_name(reloid);

					/* It's easier to construct json manually than via Jsonb API... */
					initStringInfo(&json);
					appendStringInfo(&json, "{\"schema_name\": ");
					escape_json(&json, nspname);
					switch (relkind)
					{
						case RELKIND_RELATION:
							appendStringInfo(&json, ",\"table_name\": ");
							escape_json(&json, relname);
							cmdtype = QUEUE_COMMAND_TYPE_TABLESYNC;
							break;
						case RELKIND_SEQUENCE:
							appendStringInfo(&json, ",\"sequence_name\": ");
							escape_json(&json, relname);
							appendStringInfo(&json, ",\"last_value\": \""INT64_FORMAT"\"",
											 sequence_get_last_value(reloid));
							cmdtype = QUEUE_COMMAND_TYPE_SEQUENCE;
							break;
						default:
							elog(ERROR, "unsupported relkind '%c'", relkind);
					}
					appendStringInfo(&json, "}");

					/* Queue the truncate for replication. */
					queue_message(list_make1(repset->name), GetUserId(), cmdtype,
								  json.data);
				}
			}
		}

		systable_endscan(sysscan);
	}

	table_close(rel, RowExclusiveLock);

	PG_RETURN_BOOL(true);
}

/*
 * Add replication set / table mapping based on schemas.
 */
Datum
spock_replication_set_add_all_tables(PG_FUNCTION_ARGS)
{
	Name		repset_name = PG_GETARG_NAME(0);
	ArrayType  *nsp_names = PG_GETARG_ARRAYTYPE_P(1);
	bool		synchronize = PG_GETARG_BOOL(2);

	return spock_replication_set_add_all_relations(repset_name, nsp_names,
													   synchronize,
													   RELKIND_RELATION);
}

/*
 * Add replication set / sequence mapping based on schemas.
 */
Datum
spock_replication_set_add_all_sequences(PG_FUNCTION_ARGS)
{
	Name		repset_name = PG_GETARG_NAME(0);
	ArrayType  *nsp_names = PG_GETARG_ARRAYTYPE_P(1);
	bool		synchronize = PG_GETARG_BOOL(2);

	return spock_replication_set_add_all_relations(repset_name, nsp_names,
													   synchronize,
													   RELKIND_SEQUENCE);
}

/*
 * Remove replication set / table mapping.
 *
 * Unlike the spock_replication_set_add_table, this function does not care
 * if table is valid or not, as we are just removing the record from repset.
 */
Datum
spock_replication_set_remove_table(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(1);
	SpockRepSet    *repset;
	SpockLocalNode *node;

	node = check_local_node(true);

	/* Find the replication set. */
	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*PG_GETARG_NAME(0)), false);

	replication_set_remove_table(repset->id, reloid, false);

	PG_RETURN_BOOL(true);
}

/*
 * Remove replication set / sequence mapping.
 */
Datum
spock_replication_set_remove_sequence(PG_FUNCTION_ARGS)
{
	Oid			seqoid = PG_GETARG_OID(1);
	SpockRepSet    *repset;
	SpockLocalNode *node;

	node = check_local_node(true);

	/* Find the replication set. */
	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*PG_GETARG_NAME(0)), false);

	replication_set_remove_seq(repset->id, seqoid, false);

	PG_RETURN_BOOL(true);
}

/*
 * spock_replicate_ddl_command
 *
 * Queues the input SQL for replication.
 */
Datum
spock_replicate_ddl_command(PG_FUNCTION_ARGS)
{
	text	   *command = PG_GETARG_TEXT_PP(0);
	char	   *query = text_to_cstring(command);
	int			save_nestlevel;
	List	   *replication_sets;
	ListCell   *lc;
	SpockLocalNode *node;
	StringInfoData		cmd;

	node = check_local_node(false);

	/* XXX: This is here for backwards compatibility with pre 1.1 extension. */
	if (PG_NARGS() < 2)
	{
		replication_sets = list_make1(DDL_SQL_REPSET_NAME);
	}
	else
	{
		ArrayType  *rep_set_names = PG_GETARG_ARRAYTYPE_P(1);
		replication_sets = textarray_to_list(rep_set_names);
	}

	/* Validate replication sets. */
	foreach(lc, replication_sets)
	{
		char   *setname = lfirst(lc);

		(void) get_replication_set_by_name(node->node->id, setname, false);
	}

	save_nestlevel = NewGUCNestLevel();

	/* Force everything in the query to be fully qualified. */
	(void) set_config_option("search_path", "",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0
							 , false
							 );

	/* Convert the query to json string. */
	initStringInfo(&cmd);
	escape_json(&cmd, query);

	/* Queue the query for replication. */
	queue_message(replication_sets, GetUserId(),
				  QUEUE_COMMAND_TYPE_SQL, cmd.data);

	/*
	 * Execute the query locally.
	 * Use PG_TRY to ensure in_spock_replicate_ddl_command gets cleaned up
	 */
	in_spock_replicate_ddl_command = true;
	PG_TRY();
	{
		spock_execute_sql_command(query, GetUserNameFromId(GetUserId()
															   , false
															   ),
									  false);
	}
	PG_CATCH();
	{
		in_spock_replicate_ddl_command = false;
		PG_RE_THROW();
	}
	PG_END_TRY();

	in_spock_replicate_ddl_command = false;

	/*
	 * Restore the GUC variables we set above.
	 */
	AtEOXact_GUC(true, save_nestlevel);

	PG_RETURN_BOOL(true);
}

/*
 * spock_queue_trigger
 *
 * Trigger which queues the TRUNCATE command.
 *
 * This function only writes to internal linked list, actual queueing is done
 * by spock_finish_truncate().
 */
Datum
spock_queue_truncate(PG_FUNCTION_ARGS)
{
	TriggerData	   *trigdata = (TriggerData *) fcinfo->context;
	const char	   *funcname = "queue_truncate";
	MemoryContext	oldcontext;
	SpockLocalNode *local_node;

	/* Return if this function was called from apply process. */
	if (MySpockWorker)
		PG_RETURN_VOID();

	/* Make sure this is being called as an AFTER TRUNCTATE trigger. */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager",
						funcname)));

	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) ||
		!TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired AFTER TRUNCATE",
						funcname)));

	/* If this is not spock node, don't do anything. */
	local_node = get_local_node(false, true);
	if (!local_node)
		PG_RETURN_VOID();

	/* Make sure the list change survives the trigger call. */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);
	spock_truncated_tables = lappend_oid(spock_truncated_tables,
									RelationGetRelid(trigdata->tg_relation));
	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}

/*
 * spock_dependency_check_trigger
 *
 * No longer used, present for smoother upgrades.
 */
Datum
spock_dependency_check_trigger(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();
}

Datum
spock_node_info(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5];
	HeapTuple	htup;
	char		sysid[32];
	List	   *repsets;
	SpockLocalNode *node;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	node = get_local_node(false, false);

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());
	repsets = get_node_replication_sets(node->node->id);

	memset(nulls, 0, sizeof(nulls));
	values[0] = ObjectIdGetDatum(node->node->id);
	values[1] = CStringGetTextDatum(node->node->name);
	values[2] = CStringGetTextDatum(sysid);
	values[3] = CStringGetTextDatum(get_database_name(MyDatabaseId));
	values[4] = CStringGetTextDatum(stringlist_to_identifierstr(repsets));

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * Get replication info about table.
 *
 * This is called by downstream sync worker on the upstream to obtain
 * info needed to do initial synchronization correctly. Be careful
 * about changing it, as it must be upward- and downward-compatible.
 *
 * it's not used starting with spock 2.3 (and can be remove if we drop
 * support to pre-2.3 in the future.
 *
 * It will return only tables with same name and schema on subscriber and
 * provider
 */
Datum
spock_show_repset_table_info(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	ArrayType  *rep_set_names =	PG_GETARG_ARRAYTYPE_P(1);
	Relation	rel;
	List	   *replication_sets;
	TupleDesc	reldesc;
	TupleDesc	rettupdesc;
	int			i;
	List	   *att_list = NIL;
	Datum		values[7];
	bool		nulls[7];
	char	   *nspname;
	char	   *relname;
	HeapTuple	htup;
	SpockLocalNode *node;
	SpockTableRepInfo *tableinfo;

	node = get_local_node(false, false);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &rettupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	rettupdesc = BlessTupleDesc(rettupdesc);

	rel = table_open(reloid, AccessShareLock);
	reldesc = RelationGetDescr(rel);
	replication_sets = textarray_to_list(rep_set_names);
	replication_sets = get_replication_sets(node->node->id,
											replication_sets,
											false);

	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);

	/* Build the replication info for the table. */
	tableinfo = get_table_replication_info(node->node->id, rel,
												  replication_sets);

	/* Build the column list. */
	for (i = 0; i < reldesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(reldesc,i);

		/* Skip dropped columns. */
		if (att->attisdropped)
			continue;

		/* Skip filtered columns if any. */
		if (tableinfo->att_list &&
			!bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						   tableinfo->att_list))
			continue;

		att_list = lappend(att_list, NameStr(att->attname));
	}

	/* And now build the result. */
	memset(nulls, false, sizeof(nulls));
	values[0] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[1] = CStringGetTextDatum(nspname);
	values[2] = CStringGetTextDatum(relname);
	values[3] = PointerGetDatum(strlist_to_textarray(att_list));
	values[4] = BoolGetDatum(list_length(tableinfo->row_filter) > 0);
	values[5] = CStringGetTextDatum(tableinfo->nsptarget);
	values[6] = CStringGetTextDatum(tableinfo->reltarget);

	htup = heap_form_tuple(rettupdesc, values, nulls);

	table_close(rel, NoLock);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

/*
 * Get replication info about table, by target name
 *
 * This is called by downstream sync worker on the upstream to obtain
 * info needed to do initial synchronization correctly. Be careful
 * about changing it, as it must be upward- and downward-compatible.
 */
Datum
spock_show_repset_table_info_by_target(PG_FUNCTION_ARGS)
{
	ArrayType  *rep_set_names = PG_GETARG_ARRAYTYPE_P(2);
	RangeVar   *target;
	char	   *nsptarget;
	char	   *reltarget;
	Relation	rel;
	List	   *replication_sets;
	TupleDesc	reldesc;
	int			i;
	Datum		values[7];
	bool		nulls[7];
	char	   *nspname;
	char	   *relname;
	SpockLocalNode *node;
	List *tablesinfo = NIL;
	ListCell		   *lc;
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;

	if (PG_ARGISNULL(0))
			elog(ERROR,"Schema target name required");
	nsptarget = NameStr(*PG_GETARG_NAME(0));
	if (PG_ARGISNULL(1))
			elog(ERROR,"Table target name required");
	reltarget = NameStr(*PG_GETARG_NAME(1));

	node = get_local_node(false, false);

	target = makeRangeVar(nsptarget, reltarget, -1);

	replication_sets = textarray_to_list(rep_set_names);
	replication_sets = get_replication_sets(node->node->id,
											replication_sets,
											false);

	/* Build the replication info for the table. */
	tablesinfo = get_table_replication_info_by_target(node->node->id,
													  target->schemaname,
													  target->relname,
													  replication_sets);

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	foreach (lc, tablesinfo)
	{
			List	   *att_list = NIL;
			SpockTableRepInfo *tableinfo = (SpockTableRepInfo *) lfirst(lc);

			rel = table_open(tableinfo->reloid, AccessShareLock);
			reldesc = RelationGetDescr(rel);

			nspname = get_namespace_name(RelationGetNamespace(rel));
			relname = RelationGetRelationName(rel);

			/* Build the column list. */
			for (i = 0; i < reldesc->natts; i++)
			{
					Form_pg_attribute att = TupleDescAttr(reldesc,i);

					/* Skip dropped columns. */
					if (att->attisdropped)
							continue;

					/* Skip filtered columns if any. */
					if (tableinfo->att_list &&
						!bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
									   tableinfo->att_list))
							continue;

					att_list = lappend(att_list, NameStr(att->attname));
			}

			/* And now build the result. */
			memset(nulls, false, sizeof(nulls));
			values[0] = ObjectIdGetDatum(RelationGetRelid(rel));
			values[1] = CStringGetTextDatum(nspname);
			values[2] = CStringGetTextDatum(relname);
			values[3] = PointerGetDatum(strlist_to_textarray(att_list));
			values[4] = BoolGetDatum(list_length(tableinfo->row_filter) > 0);
			values[5] = CStringGetTextDatum(tableinfo->nsptarget);
			values[6] = CStringGetTextDatum(tableinfo->reltarget);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			table_close(rel, NoLock);
	}

	// XXX not required
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/*
 * Decide if to return tuple or not.
 */
static bool
filter_tuple(HeapTuple htup, ExprContext *econtext, List *row_filter_list)
{
	ListCell	   *lc;

	ExecStoreHeapTuple(htup, econtext->ecxt_scantuple, false);

	foreach (lc, row_filter_list)
	{
		ExprState  *exprstate = (ExprState *) lfirst(lc);
		Datum		res;
		bool		isnull;

		res = ExecEvalExpr(exprstate, econtext, &isnull, NULL);

		/* NULL is same as false for our use. */
		if (isnull)
			return false;

		if (!DatumGetBool(res))
			return false;
	}

	return true;
}

/*
 * Do sequential table scan and return all rows that pass the row filter(s)
 * defined in speficied replication set(s) for a table.
 *
 * This is called by downstream sync worker on the upstream to obtain
 * filtered data for initial COPY.
 */
Datum
spock_table_data_filtered(PG_FUNCTION_ARGS)
{
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid			reloid;
 	ArrayType  *rep_set_names;
	ReturnSetInfo *rsi;
	Relation	rel;
	List	   *replication_sets;
	ListCell   *lc;
	TupleDesc	tupdesc;
	TupleDesc	reltupdesc;
	TableScanDesc scandesc;
	HeapTuple	htup;
	List	   *row_filter_list = NIL;
	EState		   *estate;
	ExprContext	   *econtext;
	Tuplestorestate *tupstore;
	SpockLocalNode *node;
	SpockTableRepInfo *tableinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	node = get_local_node(false, false);

	/* Validate parameter. */
	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));
	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("repsets cannot be NULL")));

	reloid = PG_GETARG_OID(1);
	rep_set_names = PG_GETARG_ARRAYTYPE_P(2);

	if (!type_is_rowtype(argtype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("first argument of %s must be a row type",
						"spock_table_data_filtered")));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsi->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * get the tupdesc from the result set info - it must be a record type
	 * because we already checked that arg1 is a record type, or we're in a
	 * to_record function which returns a setof record.
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));
	tupdesc = BlessTupleDesc(tupdesc);

	/* Prepare output tuple store. */
	tupstore = tuplestore_begin_heap(false, false, work_mem);
	rsi->returnMode = SFRM_Materialize;
	rsi->setResult = tupstore;
	rsi->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* Check output type and table row type are the same. */
	rel = table_open(reloid, AccessShareLock);
	reltupdesc = RelationGetDescr(rel);
	if (!equalTupleDescs(tupdesc, reltupdesc))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("return type of %s must be same as row type of the relation",
						"spock_table_data_filtered")));

	/* Build the replication info for the table. */
	replication_sets = textarray_to_list(rep_set_names);
	replication_sets = get_replication_sets(node->node->id,
											replication_sets,
											false);
	tableinfo = get_table_replication_info(node->node->id, rel,
										   replication_sets);

	/* Prepare executor. */
	estate = create_estate_for_relation(rel, false);
	econtext = prepare_per_tuple_econtext(estate, reltupdesc);

	/* Prepare the row filter expression. */
	foreach (lc, tableinfo->row_filter)
	{
		Node	   *row_filter = (Node *) lfirst(lc);
		ExprState  *exprstate = spock_prepare_row_filter(row_filter);

		row_filter_list = lappend(row_filter_list, exprstate);
	}


	/* Scan the table. */
	scandesc = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);

	while (HeapTupleIsValid(htup = heap_getnext(scandesc, ForwardScanDirection)))
	{
		if (!filter_tuple(htup, econtext, row_filter_list))
			continue;

		tuplestore_puttuple(tupstore, htup);
	}

	/* Cleanup. */
	ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
	FreeExecutorState(estate);

	heap_endscan(scandesc);
	table_close(rel, NoLock);

	PG_RETURN_NULL();
}



/*
 * Wait for subscription and initial sync to complete, or, if relation info is
 * given, for sync to complete for a specific table.
 *
 * We have to play games with snapshots to achieve this, since we're looking at
 * spock tables in the future as far as our snapshot is concerned.
 */
static void
spock_wait_for_sync_complete(char *subscription_name, char *relnamespace, char *relname)
{
	SpockSubscription *sub;

	/*
	 * If we wait in SERIALIZABLE, then the next snapshot after we return
	 * won't reflect the new state.
	 */
	if (IsolationUsesXactSnapshot())
		elog(ERROR, "cannot wait for sync in REPEATABLE READ or SERIALIZABLE isolation");

	sub = get_subscription_by_name(subscription_name, false);

	do
	{
		SpockSyncStatus	   *subsync;
		List				   *tables;
		bool					isdone = false;
		int						rc;

		/* We need to see the latest rows */
		PushActiveSnapshot(GetLatestSnapshot());

		subsync = get_subscription_sync_status(sub->id, true);
		isdone = subsync && subsync->status == SYNC_STATUS_READY;
		free_sync_status(subsync);

		if (isdone)
		{
			/*
			 * Subscription itself is synced, but what about separately
			 * synced tables?
			 */
			if (relname != NULL)
			{
				SpockSyncStatus *table = get_table_sync_status(sub->id, relnamespace, relname, false);
				isdone = table && table->status == SYNC_STATUS_READY;
				free_sync_status(table);
			}
			else
			{
				/*
				 * XXX This is plenty inefficient and we should probably just do a direct catalog
				 * scan, but meh, it hardly has to be fast.
				 */
				ListCell *lc;
				tables = get_unsynced_tables(sub->id);
				isdone = tables == NIL;
				foreach (lc, tables)
				{
					SpockSyncStatus *table = lfirst(lc);
					free_sync_status(table);
				}
				list_free(tables);
			}
		}

		PopActiveSnapshot();

		if (isdone)
			break;

		CHECK_FOR_INTERRUPTS();

		/* some kind of backoff could be useful here */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 200L);

		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		ResetLatch(&MyProc->procLatch);
	} while (1);
}

Datum
spock_wait_for_subscription_sync_complete(PG_FUNCTION_ARGS)
{
	char *subscription_name = NameStr(*PG_GETARG_NAME(0));

	spock_wait_for_sync_complete(subscription_name, NULL, NULL);

	PG_RETURN_VOID();
}

Datum
spock_wait_for_table_sync_complete(PG_FUNCTION_ARGS)
{
	char *subscription_name = NameStr(*PG_GETARG_NAME(0));
	Oid relid = PG_GETARG_OID(1);
	char *relname, *relnamespace;

	relname = get_rel_name(relid);
	relnamespace = get_namespace_name(get_rel_namespace(relid));

	spock_wait_for_sync_complete(subscription_name, relnamespace, relname);

	PG_RETURN_VOID();
}

/*
 * Like pg_xact_commit_timestamp but extended for replorigin
 * too.
 */
Datum
spock_xact_commit_timestamp_origin(PG_FUNCTION_ARGS)
{
#ifdef HAVE_REPLICATION_ORIGINS
	TransactionId xid = PG_GETARG_UINT32(0);
	TimestampTz ts;
	RepOriginId	origin;
	bool		found;
#endif
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {false, false};
	HeapTuple	tup;

	/*
	 * Construct a tuple descriptor for the result row. Must match the
	 * function declaration.
	 */
	tupdesc = CreateTemplateTupleDesc(2, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "timestamp",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "roident",
					   OIDOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

#ifdef HAVE_REPLICATION_ORIGINS
	found = TransactionIdGetCommitTsData(xid, &ts, &origin);

	if (found)
	{
		values[0] = TimestampTzGetDatum(ts);
		values[1] = ObjectIdGetDatum(origin);
	}
	else
#endif
	{
		values[0] = (Datum)0;
		nulls[0] = true;
		values[1] = (Datum)0;
		nulls[1] = true;
	}

	tup = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tup));
}

Datum
spock_gen_slot_name(PG_FUNCTION_ARGS)
{
	char	   *dbname = NameStr(*PG_GETARG_NAME(0));
	char	   *provider_node_name = NameStr(*PG_GETARG_NAME(1));
	char	   *subscription_name = NameStr(*PG_GETARG_NAME(2));
	Name		slot_name;

	slot_name = (Name) palloc0(NAMEDATALEN);

	gen_slot_name(slot_name, dbname, provider_node_name,
				  subscription_name);

	PG_RETURN_NAME(slot_name);
}


/*
 * Generate slot name (used also for origin identifier)
 *
 * The current format is:
 * spk_<subscriber database name>_<provider node name>_<subscription name>
 *
 * Note that we want to leave enough free space for 8 bytes of suffix
 * which in practice means 9 bytes including the underscore.
 */
static void
gen_slot_name(Name slot_name, char *dbname, const char *provider_node,
			  const char *subscription_name)
{
	char *cp;

	memset(NameStr(*slot_name), 0, NAMEDATALEN);
	snprintf(NameStr(*slot_name), NAMEDATALEN,
			 "spk_%s_%s_%s",
			 shorten_hash(dbname, 16),
			 shorten_hash(provider_node, 16),
			 shorten_hash(subscription_name, 16));
	NameStr(*slot_name)[NAMEDATALEN-1] = '\0';

	/* Replace all the invalid characters in slot name with underscore. */
	for (cp = NameStr(*slot_name); *cp; cp++)
	{
		if (!((*cp >= 'a' && *cp <= 'z')
			  || (*cp >= '0' && *cp <= '9')
			  || (*cp == '_')))
		{
			*cp = '_';
		}
	}
}

Datum
spock_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(SPOCK_VERSION));
}

Datum
spock_version_num(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(SPOCK_VERSION_NUM);
}

Datum
spock_max_proto_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(SPOCK_MAX_PROTO_VERSION_NUM);
}

Datum
spock_min_proto_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(SPOCK_MIN_PROTO_VERSION_NUM);
}

/* Dummy functions for backward comptibility. */
Datum
spock_truncate_trigger_add(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();
}

PGDLLEXPORT extern Datum spock_hooks_setup(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(spock_hooks_setup);
Datum
spock_hooks_setup(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();
}
