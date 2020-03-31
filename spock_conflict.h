/*-------------------------------------------------------------------------
 *
 * spock_conflict.h
 *		spock conflict detection and resolution
 *
 * Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		spock_conflict.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPOCK_CONFLICT_H
#define SPOCK_CONFLICT_H

#include "nodes/execnodes.h"

#include "replication/origin.h"

#include "utils/guc.h"

#include "spock_proto_native.h"

typedef enum SpockConflictResolution
{
	SpockResolution_ApplyRemote,
	SpockResolution_KeepLocal,
	SpockResolution_Skip
} SpockConflictResolution;

typedef enum
{
	SPOCK_RESOLVE_ERROR,
	SPOCK_RESOLVE_APPLY_REMOTE,
	SPOCK_RESOLVE_KEEP_LOCAL,
	SPOCK_RESOLVE_LAST_UPDATE_WINS,
	SPOCK_RESOLVE_FIRST_UPDATE_WINS
} SpockResolveOption;

extern int spock_conflict_resolver;
extern int spock_conflict_log_level;

typedef enum SpockConflictType
{
	CONFLICT_INSERT_INSERT,
	CONFLICT_UPDATE_UPDATE,
	CONFLICT_UPDATE_DELETE,
	CONFLICT_DELETE_DELETE
} SpockConflictType;

extern bool spock_tuple_find_replidx(EState *estate,
										 SpockTupleData *tuple,
										 TupleTableSlot *oldslot,
										 Oid *idxrelid);

extern Oid spock_tuple_find_conflict(EState *estate,
										 SpockTupleData *tuple,
										 TupleTableSlot *oldslot);

extern bool get_tuple_origin(HeapTuple local_tuple, TransactionId *xmin,
							 RepOriginId *local_origin, TimestampTz *local_ts);

extern bool try_resolve_conflict(Relation rel, HeapTuple localtuple,
								 HeapTuple remotetuple, HeapTuple *resulttuple,
								 SpockConflictResolution *resolution);


extern void spock_report_conflict(SpockConflictType conflict_type,
						  SpockRelation *rel,
						  HeapTuple localtuple,
						  SpockTupleData *oldkey,
						  HeapTuple remotetuple,
						  HeapTuple applytuple,
						  SpockConflictResolution resolution,
						  TransactionId local_tuple_xid,
						  bool found_local_origin,
						  RepOriginId local_tuple_origin,
						  TimestampTz local_tuple_timestamp,
						  Oid conflict_idx_id,
						  bool has_before_triggers);

extern bool spock_conflict_resolver_check_hook(int *newval, void **extra,
									   GucSource source);

#endif /* SPOCK_CONFLICT_H */
