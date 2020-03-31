/*-------------------------------------------------------------------------
 *
 * spock_proto.c
 * 		spock protocol functions
 *
 * Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  spock_proto.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "replication/reorderbuffer.h"
#include "spock_output_plugin.h"

#include "spock_output_proto.h"
#include "spock_proto_native.h"
#include "spock_proto_json.h"

SpockProtoAPI *
spock_init_api(SpockProtoType typ)
{
	SpockProtoAPI  *res = palloc0(sizeof(SpockProtoAPI));

	if (typ == SpockProtoJson)
	{
		res->write_rel = NULL;
		res->write_begin = spock_json_write_begin;
		res->write_commit = spock_json_write_commit;
		res->write_origin = NULL;
		res->write_insert = spock_json_write_insert;
		res->write_update = spock_json_write_update;
		res->write_delete = spock_json_write_delete;
		res->write_startup_message = json_write_startup_message;
	}
	else
	{
		res->write_rel = spock_write_rel;
		res->write_begin = spock_write_begin;
		res->write_commit = spock_write_commit;
		res->write_origin = spock_write_origin;
		res->write_insert = spock_write_insert;
		res->write_update = spock_write_update;
		res->write_delete = spock_write_delete;
		res->write_startup_message = write_startup_message;
	}

	return res;
}
