/*-------------------------------------------------------------------------
 *
 * spock_fe.h
 *              spock replication plugin
 *
 * Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *              spock_fe.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPOCK_FE_H
#define SPOCK_FE_H

extern int find_other_exec_version(const char *argv0, const char *target,
								   uint32 *version, char *retpath);

#endif /* SPOCK_FE_H */
