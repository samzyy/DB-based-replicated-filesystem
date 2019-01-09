/*
  mysqlfs - MySQL Filesystem
  Copyright (C) 2006 Tsukasa Hamano <code@cuspy.org>
  $Id: pool.h 55 2009-07-12 22:23:33Z chickenandporn $

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/** @file */

/**
 * The "global" variables of a filesystem: how to find the database, whether to fsck, and the
 * (test-mode) backgrounding.
 */
struct mysqlfs_opt {
    char *host;                 /**< MySQL host */
    char *user;                 /**< MySQL user */
    char *passwd;               /**< MySQL password */
    char *db;                   /**< MySQL database name */
    unsigned int port;		/**< MySQL port */
    char *socket;		/**< MySQL socket */
    unsigned int fsck;		/**< fsck boolean 1 => do fsck, 0 => don't.  Used in pool_check_mysql_setup() to call query_fsck()  */
    char *mycnf_group;		/**< Group in my.cnf to read defaults from */
    unsigned int init_conns;	/**< Number of DB connections to init on startup */
    unsigned int max_idling_conns;	/**< Maximum number of idling DB connections */
    char *logfile;		/**< filename to which local debug/log information will be written */
    int bg;			/**< (used for autotest) whether a term-less execution should background */
};

/** Initalize pool and preallocate connections */
int pool_init(struct mysqlfs_opt *opt);

/** Close all connections and cleanup pool */
void pool_cleanup();

/** Get DB connection from pool */
void *pool_get();

/** Put DB connection back to the pool */
void pool_put(void *conn);
