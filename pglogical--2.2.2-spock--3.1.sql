
ALTER SCHEMA pglogical RENAME TO spock;

ALTER TABLE spock.subscription ADD COLUMN sub_force_text_transfer boolean NOT NULL DEFAULT 'f';

CREATE FUNCTION spock.create_subscription(subscription_name name, provider_dsn text,
	    replication_sets text[] = '{default,default_insert_only,ddl_sql}', synchronize_structure text = 'none',
	    synchronize_data boolean = true, forward_origins text[] = '{all}', apply_delay interval DEFAULT '0',
	    force_text_transfer boolean = false)
RETURNS oid STRICT VOLATILE LANGUAGE c AS 'MODULE_PATHNAME', 'spock_create_subscription';

DROP FUNCTION spock.create_subscription(subscription_name name, provider_dsn text,
    replication_sets text[], synchronize_structure boolean,
    synchronize_data boolean, forward_origins text[], apply_delay interval);

ALTER TABLE spock.replication_set_table
      ADD COLUMN set_nsptarget name NOT NULL
    , ADD COLUMN set_reltarget name NOT NULL;
ALTER TABLE spock.replication_set_seq
      ADD COLUMN set_nsptarget name NOT NULL
    , ADD COLUMN set_seqtarget name NOT NULL;
DROP FUNCTION spock.show_repset_table_info(regclass, text[]);
CREATE FUNCTION spock.show_repset_table_info(relation regclass, repsets text[], OUT relid oid, OUT nspname text,
   OUT relname text, OUT att_list text[], OUT has_row_filter boolean, OUT nsptarget text, OUT reltarget text)
RETURNS record STRICT STABLE LANGUAGE c AS 'MODULE_PATHNAME', 'spock_show_repset_table_info';

CREATE FUNCTION spock.show_repset_table_info_by_target(nsptarget name, reltarget name, repsets text[], OUT relid oid, OUT nspname text,
   OUT relname text, OUT att_list text[], OUT has_row_filter boolean, OUT nsptarget text, OUT reltarget text)
RETURNS SETOF record STRICT STABLE LANGUAGE c AS 'MODULE_PATHNAME', 'spock_show_repset_table_info_by_target';

UPDATE spock.replication_set_table
  SET set_nsptarget = n.nspname
    , set_reltarget = c.relname
FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.oid = set_reloid;

UPDATE spock.replication_set_seq
  SET set_nsptarget = n.nspname
    , set_seqtarget = c.relname
FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.oid = set_seqoid;

-- a VACUUM FULL of the table above would be nice here.

DROP FUNCTION spock.replication_set_add_table(name, regclass, boolean,
     text[], text);
CREATE FUNCTION spock.replication_set_add_table(set_name name, relation regclass, synchronize_data boolean DEFAULT false,
	columns text[] DEFAULT NULL, row_filter text DEFAULT NULL, nsptarget name DEFAULT NULL, reltarget name DEFAULT NULL)
RETURNS boolean CALLED ON NULL INPUT VOLATILE LANGUAGE c AS 'MODULE_PATHNAME', 'spock_replication_set_add_table';

DROP FUNCTION spock.replication_set_add_sequence(name, regclass, boolean);
CREATE FUNCTION spock.replication_set_add_sequence(set_name name, relation regclass, synchronize_data boolean DEFAULT false, nsptarget name DEFAULT NULL, reltarget name DEFAULT NULL)
RETURNS boolean VOLATILE LANGUAGE c AS 'MODULE_PATHNAME', 'spock_replication_set_add_sequence';
