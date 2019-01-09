/*
  mysqlfs - MySQL Filesystem
  Copyright (C) 2006 Michal Ludvig <michal@logix.cz>
  $Id: pool.c 55 2009-07-12 22:23:33Z chickenandporn $

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include <fuse/fuse.h>
#ifdef HAVE_MYSQL_MYSQL_H
#include <mysql/mysql.h>
#endif
#ifdef HAVE_MYSQL_H
#include <mysql.h>
#endif

#include "query.h"
#include "pool.h"
#include "log.h"

struct mysqlfs_opt *opt;

/** Used in lifo_put() and lifo_get() to maintain a LIFO list. */
struct pool_lifo {
    struct pool_lifo	*next;		/**< next item in list */
    void		*conn;		/**< payload if this item in the list */
};

/* We have only one pool -> use global variables. */
struct pool_lifo *lifo_pool = NULL;
struct pool_lifo *lifo_unused = NULL;
static pthread_mutex_t lifo_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned int lifo_unused_cnt = 0;
unsigned int lifo_pool_cnt = 0;

/*********************************
 * Pool MySQL-specific functions *
 *********************************/

static MYSQL *pool_open_mysql_connection()
{
    MYSQL *mysql;
    my_bool reconnect = 1;

    mysql = mysql_init(NULL);
    if (!mysql) {
	log_printf(LOG_ERROR, "%s(): %s\n", __func__, strerror(ENOMEM));
        return NULL;
    }

    if (opt->mycnf_group)
	mysql_options(mysql, MYSQL_READ_DEFAULT_GROUP, opt->mycnf_group);

    if (! mysql_real_connect(mysql, opt->host, opt->user,
			     opt->passwd, opt->db,
			     opt->port, opt->socket, 0)) {
        log_printf(LOG_ERROR, "ERROR: mysql_real_connect(): %s\n",
		   mysql_error(mysql));
	mysql_close(mysql);
        return NULL;
    }

    /* Reconnect must be set *after* real_connect()! */
    mysql_options(mysql, MYSQL_OPT_RECONNECT, (char*)&reconnect);

    return mysql;
}

static void pool_close_mysql_connection(MYSQL *mysql)
{
    if (mysql)
        mysql_close(mysql);
}

static int pool_check_mysql_setup(MYSQL *mysql)
{
    int ret = 0;

    /* Check the server version.  */
    unsigned long mysql_version;
    mysql_version = mysql_get_server_version(mysql);
    if (mysql_version < MYSQL_MIN_VERSION) {
    	log_printf(LOG_ERROR, "Your server version is %s. "
    		   "Version %lu.%lu.%lu or higher is required.\n",
    		   mysql_get_server_info(mysql), 
    		   MYSQL_MIN_VERSION/10000L,
    		   (MYSQL_MIN_VERSION%10000L)/100,
    		   MYSQL_MIN_VERSION%100L);
    	ret = -ENOENT;
	goto out;
    }

    /* Create root directory if it doesn't exist. */
    ret = query_inode_full(mysql, "/", NULL, 0, NULL, NULL, NULL);
    if (ret == -ENOENT)
	ret = query_mkdir(mysql, "/", 0755, 0);
    if (ret < 0)
	goto out;

    /* Cleanup. */
    if (opt->fsck == 1) {
        ret = query_fsck(mysql);
    }

out:
    return ret;
}

/******************************************
 * Pool DB-independent (almost) functions *
 ******************************************/

static inline int lifo_put(void *conn)
{
    struct pool_lifo *ent;

    log_printf(LOG_D_POOL, "%s() <= %p\n", __func__, conn);
    pthread_mutex_lock(&lifo_mutex);
    if (lifo_unused) {
	ent = lifo_unused;
	lifo_unused = ent->next;
	lifo_unused_cnt--;
    } else {
	ent = calloc(1, sizeof(struct pool_lifo));
	if (!ent) {
	    pthread_mutex_unlock(&lifo_mutex);
	    return -ENOMEM;
	}
    }
    ent->conn = conn;
    ent->next = lifo_pool;
    lifo_pool = ent;
    lifo_pool_cnt++;
    pthread_mutex_unlock(&lifo_mutex);

    return 0;
}

static inline void *lifo_get()
{
    struct pool_lifo *ent;
    void *conn;

    pthread_mutex_lock(&lifo_mutex);
    if (lifo_pool) {
	ent = lifo_pool;
	conn = ent->conn;
	lifo_pool = ent->next;
	lifo_pool_cnt--;
	ent->next = lifo_unused;
	ent->conn = NULL;
	lifo_unused = ent;
	lifo_unused_cnt++;
    } else
	conn = NULL;
    pthread_mutex_unlock(&lifo_mutex);

    return conn;
}

int pool_init(struct mysqlfs_opt *opt_arg)
{
    int i, ret;

    log_printf(LOG_D_POOL, "%s()\n", __func__);
    opt = opt_arg;

    for (i = 0; i < opt->init_conns; i++) {
	void *conn = pool_open_mysql_connection();
	lifo_put(conn);
    }

    /* The following check should go to MySQL-specific section
     * so we can later add a whole new DB support without too
     * much trouble. But for now ... leave it here ... */
    MYSQL *mysql = pool_get();
    if (!mysql) {
	log_printf(LOG_ERROR, "Failed to connect MySQL server.\n");
	return -1;
    }

    ret = pool_check_mysql_setup(mysql);

    pool_put(mysql);

    return ret;
}

void pool_cleanup()
{
    void *conn;
    log_printf(LOG_D_POOL, "%s()...\n", __func__);
    while ((conn = lifo_get())) {
	log_printf(LOG_D_POOL, "%s(): closing conn=%p\n", __func__, conn);
	pool_close_mysql_connection(conn);
    }
}

void *pool_get()
{
    void *conn = lifo_get();
    if (!conn) {
	conn = pool_open_mysql_connection();
	log_printf(LOG_D_POOL, "%s(): Allocated new connection = %p\n", __func__, conn);
    } else
	log_printf(LOG_D_POOL, "%s(): Reused connection = %p\n", __func__, conn);

    return conn;
}

void pool_put(void *conn)
{
    log_printf(LOG_D_POOL, "%s(%p)\n", __func__, conn);

    /* This doesn't have to be mutex-protected.
     * If we close more conns or don't close some nothing
     * too bad happens. */
    if (lifo_pool_cnt >= opt->max_idling_conns)
	pool_close_mysql_connection(conn);
    else
	if (lifo_put(conn) < 0)
	    pool_close_mysql_connection(conn);
}
