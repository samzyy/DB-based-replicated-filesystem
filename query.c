/*
  mysqlfs - MySQL Filesystem
  Copyright (C) 2006 Tsukasa Hamano <code@cuspy.org>
  Copyright (C) 2006,2007 Michal Ludvig <michal@logix.cz>
  $Id: query.c 55 2009-07-12 22:23:33Z chickenandporn $

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
#include <fcntl.h>
#include <time.h>
#include <libgen.h>
#include <fuse.h>
#ifdef HAVE_MYSQL_MYSQL_H
#include <mysql/mysql.h>
#endif
#ifdef HAVE_MYSQL_H
#include <mysql.h>
#endif

#include "mysqlfs.h"
#include "query.h"
#include "log.h"

#define SQL_MAX 10240
#define INODE_CACHE_MAX 4096

static inline int lock_inode(MYSQL *mysql, long inode)
{
    // TODO
    return 0;
}

static inline int unlock_inode(MYSQL *mysql, long inode)
{
    // TODO
    return 0;
}

static struct data_blocks_info *
fill_data_blocks_info(struct data_blocks_info *info, size_t size, off_t offset)
{
    info->seq_first = offset / DATA_BLOCK_SIZE;
    info->offset_first = offset % DATA_BLOCK_SIZE;

    unsigned long  nr_following_blocks = ((info->offset_first + size) / DATA_BLOCK_SIZE);	
    info->length_first = nr_following_blocks > 0 ? DATA_BLOCK_SIZE - info->offset_first : size;

    info->seq_last = info->seq_first + nr_following_blocks;
    info->length_last = (info->offset_first + size) % DATA_BLOCK_SIZE;
    /* offset in last block (if it's a different one from the first block) 
     * is always 0 */

    return info;
}

/**
 * Get the attributes of an inode, filling in a struct stat.  This function
 * uses query_inode_full() to get the inode and nlinks of the given path, then
 * reads the inode data from the database, storing this data into the provided
 * structure.
 *
 * @return 0 if successful
 * @return -EIO if the result of mysql_query() is non-zero
 * @return -ENOENT if the inode at the give path is not found (actually, if the number of results is not exactly 1)
 * @param mysql handle to connection to the database
 * @param path pathname to check
 * @param stbuf struct stat to fill with the inode contents
 */
int query_getattr(MYSQL *mysql, const char *path, struct stat *stbuf)
{
    int ret;
    long inode, nlinks;
    char sql[SQL_MAX];
    MYSQL_RES* result;
    MYSQL_ROW row;
    ret = query_inode_full(mysql, path, NULL, 0, &inode, NULL, &nlinks);
    if (ret < 0)
      return ret;

    snprintf(sql, SQL_MAX,
             "SELECT inode, mode, uid, gid, ctime, atime, mtime, size "
             "FROM inodes WHERE inode=%ld",
             inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "ERROR: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    result = mysql_store_result(mysql);
    if(!result){
        log_printf(LOG_ERROR, "ERROR: mysql_store_result()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    if(mysql_num_rows(result) != 1){
        mysql_free_result(result);
        return -ENOENT;
    }
    row = mysql_fetch_row(result);
    if(!row){
        log_printf(LOG_ERROR, "ERROR: mysql_fetch_row()\n");
        return -EIO;
    }

    stbuf->st_ino = inode;
    stbuf->st_mode = atoi(row[1]);
    stbuf->st_uid = atol(row[2]);
    stbuf->st_gid = atol(row[3]);
    stbuf->st_ctime = atol(row[4]);
    stbuf->st_atime = atol(row[5]);
    stbuf->st_mtime = atol(row[6]);
    stbuf->st_size = atol(row[7]);
    stbuf->st_nlink = nlinks;

    mysql_free_result(result);

    return 0;
}

/**
 * Walk the directory tree to find the inode at the given absolute path,
 * storing name, inode, parent inode, and number of links.  Last developer of
 * this function indicates that the pathname may overflow -- sounds like a
 * good testcase :)
 *
 * If any of the name, inode, parent, or nlinks are given, those values will be
 * recorded from the inode data to the given buffers.  The name is written to
 * the given name_len.
 *
 * @return 0 if successful
 * @return -EIO if the result of mysql_query() is non-zero
 * @return -ENOENT if the file at this path is not found
 * @param mysql handle to connection to the database
 * @param path (absolute) pathname of inode to find
 * @param name destination to record (relative) name of the inode (may be NULL)
 * @param name_len length of destination buffer "name"
 * @param inode where to write the inode value, if found (may be NULL)
 * @param parent where to write the parent's inode value, if found (may be NULL)
 * @param nlinks where to write the number of links to the inode, if found (may be NULL)
 */
int query_inode_full(MYSQL *mysql, const char *path, char *name, size_t name_len,
		      long *inode, long *parent, long *nlinks)
{
    long ret;
    char sql[SQL_MAX*4];
    MYSQL_RES* result;
    MYSQL_ROW row;

    int depth = 0;
    char *pathptr = strdup(path), *pathptr_saved = pathptr;
    char *nameptr, *saveptr = NULL;
    char sql_from[SQL_MAX/3], sql_where[SQL_MAX/3];
    char *sql_from_end = sql_from, *sql_where_end = sql_where;
    char esc_name[PATH_MAX];

    // TODO: Handle too long or too nested paths that don't fit in SQL_MAX!!!
    sql_from_end += snprintf(sql_from_end, SQL_MAX, "tree AS t0");
    sql_where_end += snprintf(sql_where_end, SQL_MAX, "t0.parent IS NULL");
    while ((nameptr = strtok_r(pathptr, "/", &saveptr)) != NULL) {
        if (depth++ == 0) {
	  pathptr = NULL;
	}

        if (strlen(nameptr) > 255) {
            free(pathptr_saved);
            return -ENAMETOOLONG;
        }

        mysql_real_escape_string(mysql, esc_name, nameptr, strlen(nameptr));
	sql_from_end += snprintf(sql_from_end, SQL_MAX, " LEFT JOIN tree AS t%d ON t%d.inode = t%d.parent",
		 depth, depth-1, depth);
	sql_where_end += snprintf(sql_where_end, SQL_MAX, " AND t%d.name = '%s'",
		 depth, esc_name);
    }
    free(pathptr_saved);

    // TODO: Only run subquery when pointer to nlinks != NULL, otherwise we don't need it.
    snprintf(sql, SQL_MAX, "SELECT t%d.inode, t%d.name, t%d.parent, "
	     		   "       (SELECT COUNT(*) FROM tree AS t%d WHERE t%d.inode=t%d.inode) "
			   "               AS nlinks "
	     		   "FROM %s WHERE %s",
	     depth, depth, depth, 
	     depth+1, depth+1, depth,
	     sql_from, sql_where);
    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "ERROR: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    result = mysql_store_result(mysql);
    if(!result){
        log_printf(LOG_ERROR, "ERROR: mysql_store_result()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    if(mysql_num_rows(result) != 1){
        mysql_free_result(result);
        return -ENOENT;
    }

    row = mysql_fetch_row(result);
    if(!row){
        log_printf(LOG_ERROR, "ERROR: mysql_fetch_row()\n");
        return -EIO;
    }
    log_printf(LOG_D_OTHER, "query_inode(path='%s') => %s, %s, %s, %s\n",
	       path, row[0], row[1], row[2], row[3]);
    
    if (inode)
        *inode = atol(row[0]);
    if (name)
        snprintf(name, name_len, "%s", row[1]);
    if (parent)
        *parent = row[2] ? atol(row[2]) : -1;	/* parent may be NULL */
    if (nlinks)
        *nlinks = atol(row[3]);

    mysql_free_result(result);

    return 0;
}

/**
 * Get the inode of a pathname.  This is really a convenience function wrapping
 * the query_inode_full() function, but can instead be used as a function with
 * a nestable return value.
 *
 * @return ID of inode
 * @return < 0 result of query_inode_full() if that function reports a failure
 * @param mysql handle to connection to the database
 * @param path (full) pathname of inode to find
 */
long query_inode(MYSQL *mysql, const char *path)
{
    long inode, ret;
    
    if (strlen(path) > PATH_MAX)
        return -ENAMETOOLONG;
    ret = query_inode_full(mysql, path, NULL, 0, &inode, NULL, NULL);
    if (ret < 0)
      return ret;
    return inode;
}

/**
 * Change the length of a file, truncating any additional data blocks and
 * immediately deleting the data blocks past the truncation length.  Function
 * works by deleting whole blocks past the truncation point, limiting the
 * partially-cleared block, and zeroing the extra part of the buffer.
 * Called by mysqlfs_truncate().
 *
 * @see http://linux.die.net/man/2/truncate
 *
 * @return 0 on success; non-zero return of mysql_query() on error
 * @param mysql handle to connection to the database
 * @param inode node to operate on
 * @param length new length of file
 */
int query_truncate(MYSQL *mysql, long inode, off_t length)
{
    int ret;
    char sql[SQL_MAX];
    struct data_blocks_info info;

    fill_data_blocks_info(&info, length, 0);

    lock_inode(mysql, inode);

    snprintf(sql, SQL_MAX,
             "DELETE FROM data_blocks WHERE inode=%ld AND seq > %ld",
	     inode, info.seq_last);
    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    if ((ret = mysql_query(mysql, sql))) goto err_out;

    snprintf(sql, SQL_MAX,
             "UPDATE data_blocks SET data=RPAD(data, %zu, '\\0') "
	     "WHERE inode=%ld AND seq=%ld",
             info.length_last, inode, info.seq_last);
    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    if ((ret = mysql_query(mysql, sql))) goto err_out;

    snprintf(sql, SQL_MAX,
             "UPDATE inodes SET size=%lld, mtime=UNIX_TIMESTAMP(NOW()), ctime=UNIX_TIMESTAMP(NOW()) WHERE inode=%ld",
             (long long)length, inode);
    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    if ((ret = mysql_query(mysql, sql))) goto err_out;

    unlock_inode(mysql, inode);

    return 0;

err_out:
    unlock_inode(mysql, inode);
    log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
    return ret;
}

/**
 * The opposite of query_rmdirentry(), this function creates a directory in
 * the tree with given inode and parent inode.
 *
 * @return 0 if successful
 * @return -EIO if the result of mysql_query() is non-zero
 * @param mysql handle to connection to the database
 * @param inode inode of new directory
 * @param name name (relative)  of directory to create
 * @param parent inode of directory holding the directory
 */
int query_mkdirentry(MYSQL *mysql, long inode, const char *name, long parent)
{
    int ret;
    char sql[SQL_MAX];
    char esc_name[PATH_MAX * 2];

    /* 
     * Should really update ctime in inode --- but if we did that we could 
     * add an nlinks count in the inode relation and get rid of the funcky join
     */
    mysql_real_escape_string(mysql, esc_name, name, strlen(name));
    snprintf(sql, SQL_MAX,
             "INSERT INTO tree (name, parent, inode) VALUES ('%s', %ld, %ld)",
             esc_name, parent, inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    ret = mysql_query(mysql, sql);
    if(ret) {
      log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
      return -EIO;
    }

    return 0;
}

/**
 * The opposite of query_mkdirentry(), this function deletes a link
 * from the tree with a parent that matches the inode given.
 * It will refuse to delete an entry if it has children (i.e. is  a 
 * non-empty directory)
 *
 * @return 0 if successful
 * @return -EIO if the result of mysql_query() is non-zero
 * @param mysql handle to connection to the database
 * @param name name (relative)  of directory to delete
 * @param parent inode of directory holding the directory
 */
int query_rmdirentry(MYSQL *mysql, const char *name, long inode, long parent)
{
    int ret;
    MYSQL_RES *result;
    char sql[SQL_MAX];
    char esc_name[PATH_MAX * 2];

    snprintf(sql, SQL_MAX, "select inode from tree where parent = %ld\n", inode);
    ret = mysql_query(mysql, sql);
    if(ret) {
        log_printf(LOG_ERROR, "ERROR: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    result = mysql_store_result(mysql);
    if(!result){
        log_printf(LOG_ERROR, "ERROR: mysql_store_result()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    if (mysql_num_rows(result) != 0)
        return -ENOTEMPTY;
    
    mysql_real_escape_string(mysql, esc_name, name, strlen(name));
    snprintf(sql, SQL_MAX,
             "DELETE FROM tree WHERE name='%s' AND parent=%ld",
             esc_name, parent);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    ret = mysql_query(mysql, sql);
    if(ret) {
      log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
      return -EIO;
    }

    return 0;
}

/**
 * Create an inode.  This function creates a child entry of the specified dev_t
 * type and mode in the "parent" directory given as the "parent".  Any parent
 * directory information (ie "dirname(path)") is stripped out, leaving only
 * the base pathname, but it has to be there (perhaps a bug?) since this
 * function wants to strip out the path information that might conflict with
 * the parent node's pathname.
 *
 * @see http://linux.die.net/man/2/mknod
 *
 * @return ID of new inode, or -ENOENT if the path contains no parent directory "/"
 * @param mysql handle to connection to the database
 * @param path name of directory to create
 * @param mode access mode of new directory
 * @param rdev type of inode to create
 * @param parent inode of directory holding files (parent inode)
 * @param alloc_data (unused)
 */
long query_mknod(MYSQL *mysql, const char *path, mode_t mode, dev_t rdev,
                long parent, int alloc_data)
{
    int ret;
    char sql[SQL_MAX];
    long new_inode_number = 0;
    char *name, esc_name[PATH_MAX * 2];

    if (path[0] == '/' && path[1] == '\0')  {
        snprintf(sql, SQL_MAX,
                 "INSERT INTO tree (name, parent) VALUES ('/', NULL)");

        log_printf(LOG_D_SQL, "sql=%s\n", sql);
        ret = mysql_query(mysql, sql);
        if(ret)
          goto err_out;
    } else {
        name = strrchr(path, '/');
        if (!name || *++name == '\0') 
            return -ENOENT;
        if (strlen(name) > 255)
            return -ENAMETOOLONG;

        mysql_real_escape_string(mysql, esc_name, name, strlen(name));
        snprintf(sql, SQL_MAX,
                 "INSERT INTO tree (name, parent) VALUES ('%s', %ld)",
                 esc_name, parent);

        log_printf(LOG_D_SQL, "sql=%s\n", sql);
        ret = mysql_query(mysql, sql);
        if(ret)
          goto err_out;
    }

    new_inode_number = mysql_insert_id(mysql);

    snprintf(sql, SQL_MAX,
             "INSERT INTO inodes(inode, mode, uid, gid, atime, ctime, mtime)"
             "VALUES(%ld, %d, %d, %d, UNIX_TIMESTAMP(NOW()), "
	            "UNIX_TIMESTAMP(NOW()), UNIX_TIMESTAMP(NOW()))",
             new_inode_number, mode,
	     fuse_get_context()->uid, fuse_get_context()->gid);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    ret = mysql_query(mysql, sql);
    if(ret)
      goto err_out;

    return new_inode_number;

err_out:
    log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
    return ret;
}

/**
 * Create a directory.  This is really a wrapper to a specific invocation of query_mknod().
 *
 * @see http://linux.die.net/man/2/mkdir
 *
 * @return ID of new inode, or -ENOENT if the path contains no parent directory "/"
 * @param mysql handle to connection to the database
 * @param path name of directory to create
 * @param mode access mode of new directory
 * @param parent inode of directory holding files (parent inode)
 */
long query_mkdir(MYSQL *mysql, const char *path, mode_t mode, long parent)
{
    return query_mknod(mysql, path, S_IFDIR | mode, 0, parent, 0);
}

/**
 * Read a directory.  This is done by listing the nodes with a given node as
 * parent, calling the filler parameter (pointer-to-function) for each item.
 * The set of results is not ordered, so results would be in the "natural order"
 * of the database.
 *
 * @see http://linux.die.net/man/2/readdir
 *
 * @return 0 on success; -EIO on failure (non-zero return from mysql_query() function)
 * @param mysql handle to connection to the database
 * @param inode inode of directory holding files (parent inode)
 * @param buf buffer to pass to filler function
 * @param filler fuse_fill_dir_t function-pointer used to process each directory entry
 */
int query_readdir(MYSQL *mysql, long inode, void *buf, fuse_fill_dir_t filler, int flag)
{
    int ret;
    char sql[SQL_MAX];
    MYSQL_RES* result;
    MYSQL_ROW row;
    struct stat st;

    snprintf(sql, sizeof(sql), "SELECT tree.name, tree.inode, inodes.mode FROM tree inner join inodes on tree.inode = inodes.inode WHERE tree.parent = '%ld'",
             inode);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    result = mysql_store_result(mysql);
    if(!result){
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    memset(&st, 0, sizeof st);
    while((row = mysql_fetch_row(result)) != NULL){
        st.st_ino = atol(row[1]);
        st.st_mode = atol(row[2]);
        filler(buf, (char*)basename(row[0]), &st, 0, 0);
    }

    mysql_free_result(result);

    return 0;
}

/**
 * Change the mode attribute in the inode entry.  Should be the entry-point
 * for the kernel's implementation of a chmod() call in an inode on the FUSE
 * filesystem.
 *
 * @see http://linux.die.net/man/2/chmod
 *
 * @return 0 on success; -EIO on failure (non-zero return from mysql_query() function)
 * @param mysql handle to connection to the database
 * @param inode inode to update
 * @param mode new mode to set into the inode
 */
int query_chmod(MYSQL *mysql, long inode, mode_t mode)
{
    int ret;
    char sql[SQL_MAX];

    snprintf(sql, SQL_MAX,
             "UPDATE inodes SET ctime=UNIX_TIMESTAMP(NOW()), mode=%d WHERE inode=%ld",
             mode, inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    return 0;
}

/**
 * Change the uid, gid attributes in the inode entry.  Should be
 * the entry-point for the kernel's implementation of a chown() call in an inode
 * on the FUSE filesystem.
 *
 * @see http://linux.die.net/man/2/chown
 *
 * @return 0 on success; -EIO on failure (non-zero return from mysql_query() function)
 * @param mysql handle to connection to the database
 * @param inode inode to update
 * @param uid uid to set (-1 to make no change to uid)
 * @param gid gid to set (-1 to make no change to gid)
 */
int query_chown(MYSQL *mysql, long inode, uid_t uid, gid_t gid)
{
    int ret;
    char sql[SQL_MAX];
    size_t index;

    index = snprintf(sql, SQL_MAX, "UPDATE inodes SET ctime=UNIX_TIMESTAMP(NOW()),");
    if (uid != (uid_t)-1)
    	index += snprintf(sql + index, SQL_MAX - index, 
			  "uid=%d ", uid);
    if (gid != (gid_t)-1)
    	index += snprintf(sql + index, SQL_MAX - index,
			  "%s gid=%d ", 
			  /* Insert comma if this is a second argument */
			  (uid != (uid_t)-1) ? "," : "",
			  gid);
    snprintf(sql + index, SQL_MAX - index, "WHERE inode=%ld", inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    return 0;
}

/**
 * Change the utime attributes atime and mtime in the inode entry.  Should be
 * the entry-point for the kernel's implementation of a utime() call in an inode
 * on the FUSE filesystem.
 *
 * @see http://linux.die.net/man/2/utime
 * @see http://linux.die.net/man/2/stat
 *
 * @return 0 on success; -EIO on failure (non-zero return from mysql_query() function)
 * @param mysql handle to connection to the database
 * @param inode inode to update the atime, mtime
 * @param time utimbuf with new actime, modtime, to set into access and modification times
 */
int query_utime(MYSQL *mysql, long inode, const struct timespec tv[2])
{
    int ret;
    char sql[SQL_MAX];

    snprintf(sql, SQL_MAX,
             "UPDATE inodes "
             "SET atime=%ld, mtime=%ld "
             "WHERE inode=%lu",
             tv[0].tv_sec, tv[1].tv_sec, inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    return 0;
}

/**
 * Read a number of bytes (perhaps larger than BLOCK_SIZE) at an offset from
 * a file.  The function does this by reading each block in succession, copying
 * the block contents into the target buffer.  The (offset % DATA_BLOCK_SIZE)
 * issue is handled by shifting the copy slightly.
 *
 * @return < 0 in case of errors (propagating result of write_one_block() )
 * @return > 0 number of bytes read (should equal size parameter)
 * @param mysql handle to connection to the database
 * @param inode inode of the file in question
 * @param buf the buffer to copy read bytes
 * @param size number of bytes to read
 * @param offset offset within the file to read from
 */
int query_read(MYSQL *mysql, long inode, const char *buf, size_t size,
               off_t offset)
{
    int ret;
    char sql[SQL_MAX];
    MYSQL_RES* result;
    MYSQL_ROW row;
    unsigned long length = 0L, copy_len, seq;
    struct data_blocks_info info;
    char *dst = (char *)buf;
    char *src, *zeroes = alloca(DATA_BLOCK_SIZE);

    fill_data_blocks_info(&info, size, offset);

    /* Read all required blocks */
    snprintf(sql, SQL_MAX,
             "SELECT seq, data, LENGTH(data) FROM data_blocks WHERE inode=%ld AND seq>=%lu AND seq <=%lu ORDER BY seq ASC",
             inode, info.seq_first, info.seq_last);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "ERROR: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    result = mysql_store_result(mysql);
    if(!result){
        log_printf(LOG_ERROR, "ERROR: mysql_store_result()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    /* This is a bit tricky as we support 'sparse' files now.
     * It means not all requested blocks must exist in the
     * database. For those that don't exist we'll return
     * a block of \0 instead.  */
    row = mysql_fetch_row(result);
    memset(zeroes, 0L, DATA_BLOCK_SIZE);
    for (seq = info.seq_first; seq<=info.seq_last; seq++) {
        off_t row_seq = -1;
	size_t row_len = DATA_BLOCK_SIZE;
	char *data = zeroes;

	if (row && (row_seq = atoll(row[0])) == seq) {
	    data = row[1];
	    row_len = atoll(row[2]);
	}
	    
	if (seq == info.seq_first) {
	    if (row_len < info.offset_first)
	        goto go_away;

	    copy_len = MIN(row_len - info.offset_first, info.length_first);
	    src = data + info.offset_first;
	} else if (seq == info.seq_last) {
	    copy_len = MIN(info.length_last, row_len);
	    src = data;
	} else {
	    copy_len = MIN(DATA_BLOCK_SIZE, row_len);
	    src = data;
	}

	memcpy(dst, src, copy_len);
	dst += copy_len;
	length += copy_len;

	if (row && row_seq == seq)
	    row = mysql_fetch_row(result);
    }

go_away:
    /* Read all remaining rows */
    while (mysql_fetch_row(result));
    mysql_free_result(result);

    return length;
}

/**
 * Writes a specific block into the database
 *
 * This function takes an early bail-out if the size to write is zero, or if the total size to write exceeds the block size.
 *
 * This function checks to see if the previous block didn't exist -- in such
 * case, it then writes out a zero-length block.  The function then creates a
 * statement that has a '?' token representing the new data.  If the previous
 * data didn't exist, the function uses a "SET x == y" format; otherwise, a
 * "CONCAT (data, ?)".  The statement is snprintf'd, prepared, and executed; the
 * result produces either a 0 on success, or a -EIO on failure (with an error
 * message logged).
 *
 * @return 0 on success; -EIO on failure
 * @param mysql handle to connection to the database
 * @param inode inode to write out the data block on
 * @param seq sequence number of datablock to write
 * @param data buffer of content to write
 * @param size size_t length of data
 * @param offset what offset within the datablock to write the data
 */
static int write_one_block(MYSQL *mysql, long inode,
				 unsigned long seq,
				 const char *data, size_t size,
				 off_t offset)
{
    MYSQL_STMT *stmt;
    MYSQL_BIND bind[1];
    char sql[SQL_MAX];
    size_t current_block_size = query_size_block(mysql, inode, seq);

    /* Shortcut */
    if (size == 0) return 0;

    if (offset + size > DATA_BLOCK_SIZE) {
        log_printf(LOG_ERROR, "%s(): offset(%zu)+size(%zu)>max_block(%d)\n", 
		   __func__, offset, size, DATA_BLOCK_SIZE);
	return -EIO;
    }

    /* We expect the inode is already locked for this thread by caller! */

    if (current_block_size == -ENXIO) {
        /* This data block has not yet been allocated */
        snprintf(sql, SQL_MAX,
                 "INSERT INTO data_blocks SET inode=%ld, seq=%lu, data=''", inode, seq);
        log_printf(LOG_D_SQL, "sql=%s\n", sql);
        if(mysql_query(mysql, sql)){
            log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
            return -EIO;
        }

        current_block_size = query_size_block(mysql, inode, seq);
    }

    stmt = mysql_stmt_init(mysql);
    if (!stmt)
    {
        log_printf(LOG_ERROR, "mysql_stmt_init(), out of memory\n");
	return -EIO;
    }

    memset(bind, 0, sizeof(bind));
    if (offset == 0 && current_block_size == 0) {
        snprintf(sql, SQL_MAX,
                 "UPDATE data_blocks "
		 "SET data=? "
		 "WHERE inode=%ld AND seq=%lu",
		 inode, seq);
    } else if (offset == current_block_size) {
        snprintf(sql, sizeof(sql),
                 "UPDATE data_blocks "
		 "SET data=CONCAT(data, ?) "
		 "WHERE inode=%ld AND seq=%lu",
		 inode, seq);
    } else {
        size_t pos;
        pos = snprintf(sql, sizeof(sql),
		 "UPDATE data_blocks SET data=CONCAT(");
	if (offset > 0)
	    pos += snprintf(sql + pos, sizeof(sql) - pos, "RPAD(IF(ISNULL(data),'', data), %llu, '\\0'),", (long long)offset);
	pos += snprintf(sql + pos, sizeof(sql) - pos, "?,");

	if (offset + size < current_block_size) {
	    pos += snprintf(sql + pos, sizeof(sql) - pos, "SUBSTRING(data FROM %llu),", (long long)offset + size + 1);
	}
	sql[--pos] = '\0';	/* Remove the trailing comma. */
	pos += snprintf(sql + pos, sizeof(sql) - pos, ") WHERE inode=%ld AND seq=%lu",
			inode, seq);
    }
    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
	log_printf(LOG_ERROR, "mysql_stmt_prepare() failed: %s\n", mysql_stmt_error(stmt));
	goto err_out;
    }

    if (mysql_stmt_param_count(stmt) != 1) {
      log_printf(LOG_ERROR, "%s(): stmt_param_count=%d, expected 1\n", __func__, mysql_stmt_param_count(stmt));
      return -EIO;
    }
    bind[0].buffer_type= MYSQL_TYPE_LONG_BLOB;
    bind[0].buffer= (char *)data;
    bind[0].is_null= 0;
    bind[0].length= (unsigned long *)(void *)&size;

    if (mysql_stmt_bind_param(stmt, bind)) {
	log_printf(LOG_ERROR, "mysql_stmt_bind_param() failed: %s\n", mysql_stmt_error(stmt));
	goto err_out;
    }

    /*
    if (!mysql_stmt_send_long_data(stmt, 0, data, size))
    {
        log_printf(" send_long_data failed");
	goto err_out;
    }
    */
    if (mysql_stmt_execute(stmt)) {
	log_printf(LOG_ERROR, "mysql_stmt_execute() failed: %s\n", mysql_stmt_error(stmt));
	goto err_out;
    }

    if (mysql_stmt_close(stmt))
	log_printf(LOG_ERROR, "failed closing the statement: %s\n", mysql_stmt_error(stmt));

    /* Update file size */
    snprintf(sql, SQL_MAX,
	     "UPDATE inodes SET size=("
	     	"SELECT seq*%d + LENGTH(data) FROM data_blocks WHERE inode=%ld AND seq=("
			"SELECT MAX(seq) FROM data_blocks WHERE inode=%ld"
		")"
	     ") "
	     "WHERE inode=%ld",
	     DATA_BLOCK_SIZE, inode, inode, inode);
    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    if(mysql_query(mysql, sql)) {
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    return size;

err_out:
	log_printf(LOG_ERROR, " %s\n", mysql_stmt_error(stmt));
	if (mysql_stmt_close(stmt))
	    log_printf(LOG_ERROR, "failed closing the statement: %s\n", mysql_stmt_error(stmt));
	return -EIO;
}

/**
 * Write a number of bytes (perhaps larger than BLOCK_SIZE) at an offset into
 * a file.  The function does this by writing the first partial block, then
 * writing successive blocks until the full @c size is written.
 *
 * @return < 0 in case of errors (propagating result of write_one_block() )
 * @return > 0 number of bytes written (should equal size parameter)
 * @param mysql handle to connection to the database
 * @param inode inode of the file in question
 * @param data the buffer of data to write
 * @param size number of bytes to write
 * @param offset offset within the file to write to
 */
int query_write(MYSQL *mysql, long inode, const char *data, size_t size,
                off_t offset)
{
    struct data_blocks_info info;
    unsigned long seq;
    const char *ptr;
    int ret, ret_size = 0;

    fill_data_blocks_info(&info, size, offset);

    /* Handle first block */
    lock_inode(mysql, inode);
    ret = write_one_block(mysql, inode, info.seq_first, data,
			  info.length_first, info.offset_first);
    unlock_inode(mysql, inode);
    if (ret < 0)
        return ret;
    ret_size = ret;

    /* Shortcut - if last block seq is the same as first block
     * seq simply go away as it's the same block */
    if (info.seq_first == info.seq_last)
        return ret_size;

    ptr = data + info.length_first;

    /* Handle all full-sized intermediate blocks */
    for (seq = info.seq_first + 1; seq < info.seq_last; seq++) {
        lock_inode(mysql, inode);
        ret = write_one_block(mysql, inode, seq, ptr, DATA_BLOCK_SIZE, 0);
        unlock_inode(mysql, inode);
        if (ret < 0)
            return ret;
	ptr += DATA_BLOCK_SIZE;
	ret_size += ret;
    }

    /* Handle last block */
    lock_inode(mysql, inode);
    ret = write_one_block(mysql, inode, info.seq_last, ptr,
			  info.length_last, 0);
    unlock_inode(mysql, inode);
    if (ret < 0)
        return ret;
    ret_size += ret;

    return ret_size;
}

/**
 * Check the size of a file.  Check the value by reading the attribute stored
 * in the inode table itself.  The function does not summarize the size "live"
 * by summing the size of each data block; rather this value is updated in
 * query_fsck(), query_truncate(), write_one_block().  This trust in the
 * various write functions optimizes this function's response time and
 * reduces DB load.
 *
 * @return total size of the file at the inode as represented in the inode block
 * @param mysql handle to connection to the database
 * @param inode inode of the file in question
 */
ssize_t query_size(MYSQL *mysql, long inode)
{
    size_t ret;
    char sql[SQL_MAX];
    MYSQL_RES *result;
    MYSQL_ROW row;

    snprintf(sql, SQL_MAX, "SELECT size FROM inodes WHERE inode=%ld",
             inode);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }
    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    result = mysql_store_result(mysql);
    if(!result){
        log_printf(LOG_ERROR, "ERROR: mysql_store_result()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    if(mysql_num_rows(result) != 1 || mysql_num_fields(result) != 1){
        mysql_free_result(result);
        log_printf(LOG_ERROR, "ERROR: non-unique number of rows for %d\n", inode);
        return -EIO;
    }

    row = mysql_fetch_row(result);
    if(!row){
        log_printf(LOG_ERROR, "ERROR: row-fetch failed for %d\n", inode);
        return -EIO;
    }

    if (row[0]) {
        ret = atoll(row[0]);
    } else {
        ret = 0;
    }
    mysql_free_result(result);

    return ret;
}

/**
 * Returns the size of the given block (inode and sequence number).  Used only by write_one_block(), which is static, so this one can/should be static?
 *
 * @return -ENXIO if the inode/seq pair is not found (zero rows returned, implying that block doesn't exist)
 * @return -EIO if no row is returned (implying an error in the query response, signaled by mysql_fetch_row() returning NULL)
 * @return 0 if the row is NULL (implying no result?)
 * @return 1 - DATA_BLOCK_SIZE (size of the actual block)
 * @param mysql handle to connection to the database
 * @param inode inode of the file in question
 * @param seq sequence number of datablock to check
 */
ssize_t query_size_block(MYSQL *mysql, long inode, unsigned long seq)
{
    size_t ret;
    char sql[SQL_MAX];
    MYSQL_RES *result;
    MYSQL_ROW row;

    snprintf(sql, SQL_MAX, "SELECT LENGTH(data) FROM data_blocks WHERE inode=%ld AND seq=%lu",
             inode, seq);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }
    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    result = mysql_store_result(mysql);
    if(!result){
        log_printf(LOG_ERROR, "ERROR: mysql_store_result()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    if(mysql_num_rows(result) == 0) {
        mysql_free_result(result);
        return -ENXIO;
    }

    row = mysql_fetch_row(result);
    if(!row){
        log_printf(LOG_ERROR, "Could not fetch row: query_size_block\n");
        return -EIO;
    }

    if(row[0]){
        ret = atoll(row[0]);
    }else{
        ret = 0;
    }
    mysql_free_result(result);

    return ret;
}

/**
 * Rename a file.  Called by mysqlfs_rename()
 *
 * @return 0 on success; -EIO if the mysql_query() is non-zero (and the error is logged)
 *
 * @see http://linux.die.net/man/2/rename
 *
 * @param mysql handle to the database
 * @param from name of file before the rename
 * @param to name of file after the rename
 */
int query_rename(MYSQL *mysql, const char *from, const char *to)
{
    int ret;
    long inode, parent_to, parent_from;
    char *tmp, *new_name, *old_name;
    char esc_new_name[PATH_MAX * 2], esc_old_name[PATH_MAX * 2];
    char sql[SQL_MAX];

    struct stat to_st;

    
    if (query_getattr(mysql, to, &to_st) != -ENOENT) {
        if (S_ISDIR(to_st.st_mode))
            return -EEXIST;
    } else {
        to_st.st_ino = 0;
    }

    inode = query_inode(mysql, from);

    /* Lots of strdup()s follow because dirname() & basename()
     * may modify the original string. */
    tmp = strdup(from);
    parent_from = query_inode(mysql, dirname(tmp));
    free(tmp);

    tmp = strdup(from);
    old_name = basename(tmp);
    mysql_real_escape_string(mysql, esc_old_name, old_name, strlen(old_name));
    free(tmp);

    tmp = strdup(to);
    parent_to = query_inode(mysql, dirname(tmp));
    free(tmp);

    tmp = strdup(to);
    new_name = basename(tmp);
    mysql_real_escape_string(mysql, esc_new_name, new_name, strlen(new_name));
    free(tmp);

    snprintf(sql, SQL_MAX,
             "UPDATE tree "
	     "SET name='%s', parent=%ld "
	     "WHERE inode=%ld AND name='%s' AND parent=%ld ",
             esc_new_name, parent_to,
	     inode, esc_old_name, parent_from);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    if (to_st.st_ino) {
        snprintf(sql, SQL_MAX, "DELETE FROM tree "
                 "WHERE inode = %lu and name = '%s' and parent='%ld'",
                 to_st.st_ino, esc_new_name, parent_to);
        log_printf(LOG_D_SQL, "sql=%s\n", sql);
        ret = mysql_query(mysql, sql);
        if (ret) {
            log_printf(LOG_ERROR, "Error: mysql_query()\n");
            log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
            return -EIO;
        }
    }

    /*
    if (mysql_affected_rows(mysql) < 1)
      return -ETHIS_IS_STRANGE;	/ * Someone deleted the direntry? Do we care? * /
    */

    return 0;
}

/**
 * Mark the file in-use: like a lock-manager, increment the count of users of
 * this file so that deletions at the inode level cannot result in purged data
 * while the file is in-use.
 *
 * @return 0 on success; -EIO if the mysql_query() is non-zero (and the error is logged)
 * @param mysql handle to the database
 * @param inode inode of the file that is to be marked deleted 
 * @param increment how many additional "uses" to increment in the file's inode
 */
int query_inuse_inc(MYSQL *mysql, long inode, int increment)
{
    int ret;
    char sql[SQL_MAX];

    snprintf(sql, SQL_MAX,
             "UPDATE inodes SET inuse = inuse + %d "
             "WHERE inode=%lu",
             increment, inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    return 0;
}

/**
 * Purge inodes from files previously marked deleted (ie query_set_deleted() )
 * and are no longer in-use.  Called by mysqlfs_unlink() and mysqlfs_release()
 *
 * @return 0 on success; -EIO if the mysql_query() is non-zero (and the error is logged)
 * @param mysql handle to the database
 * @param inode inode of the file that is to be marked deleted 
 */
int query_purge_deleted(MYSQL *mysql, long inode)
{
    int ret;
    char sql[SQL_MAX];

    snprintf(sql, SQL_MAX,
	     "DELETE FROM inodes WHERE inode=%ld AND inuse=0 AND deleted=1",
             inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    return 0;
}

/**
 * Mark the inode deleted where the name of the tree column is NULL.  This
 * allows files that are still in use to be deleted without wiping out their
 * underlying data.
 *
 * @return 0 on success; -EIO if the mysql_query() is non-zero (and the error is logged)
 * @param mysql handle to the database
 * @param inode inode of the file that is to be marked deleted
 */
int query_set_deleted(MYSQL *mysql, long inode)
{
    int ret;
    char sql[SQL_MAX];

    snprintf(sql, SQL_MAX,
	     "UPDATE inodes LEFT JOIN tree ON inodes.inode = tree.inode SET inodes.deleted=1 "
	     "WHERE inodes.inode = %ld AND tree.name IS NULL",
             inode);

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }

    return 0;
}

/**
 * Clean filesystem.  Only run in pool_check_mysql_setup() if mysqlfs_opt::fsck == 1
 *
 * -# delete inodes with deleted==1
 * -# delete direntries without corresponding inode
 * -# set inuse=0 for all inodes
 * -# delete data without existing inode
 * -# synchronize inodes.size=data.LENGTH(data)
 * -# optimize tables
 *
 * @return return from call to mysql_query()
 * @param mysql handle to database connection
 */
int query_fsck(MYSQL *mysql)
{

    /*
     query_fsck by florian wiessner (f.wiessner@smart-weblications.de)
    */
    printf("Starting fsck\n");

    // 1. delete inodes with deleted==1
    int ret;
//    int ret2;
    char sql[SQL_MAX];
    printf("Stage 1...\n");
    snprintf(sql, SQL_MAX,
             "DELETE from inodes WHERE inodes.deleted = 1");

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }
    // 2. - delete direntries without corresponding inode
    printf("Stage 2...\n");
    snprintf(sql, SQL_MAX, "delete from tree where tree.inode not in (select inode from inodes);");

    log_printf(LOG_D_SQL, "sql=%s\n", sql);
    ret = mysql_query(mysql, sql);

    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }



    // 3. set inuse=0 for all inodes
    printf("Stage 3...\n");
    snprintf(sql, SQL_MAX, "UPDATE inodes SET inuse=0;");

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }


    // 4. delete data without existing inode
    printf("Stage 4...\n");
    snprintf(sql, SQL_MAX, "delete from data_blocks where inode not in (select inode from inodes);");

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }


    // 5. synchronize inodes.size=data.LENGTH(data)
    printf("Stage 5...\n");
    long int inode;
    long int size;
    
    snprintf(sql, SQL_MAX, "select inode, sum(OCTET_LENGTH(data)) as size from data_blocks group by inode");

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);

    MYSQL_RES* myresult;
    MYSQL_ROW row;

    myresult = mysql_store_result(mysql);
    while ((row = mysql_fetch_row(myresult)) != NULL) {
     inode = atol(row[0]);
     size = atol(row[1]);
                     
      snprintf(sql, SQL_MAX, "update inodes set size=%ld where inode=%ld;", size, inode);
      log_printf(LOG_D_SQL, "sql=%s\n", sql);
      mysql_query(mysql, sql);

/*      if (myresult) { // something has gone wrong.. delete datablocks...

        snprintf(sql, SQL_MAX, "delete from inodes where inode=%ld;", inode);
        log_printf(LOG_D_SQL, "sql=%s\n", sql);
        ret2 = mysql_query(mysql, sql);

      }
*/ // skip this for now!

    }

    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
       return -EIO;
    }
    mysql_free_result(myresult);

/*    printf("optimizing tables\n");
    snprintf(sql, SQL_MAX,
             "OPTIMIZE TABLE inodes;");

    log_printf(LOG_D_SQL, "sql=%s\n", sql);

    ret = mysql_query(mysql, sql);
    if(ret){
        log_printf(LOG_ERROR, "Error: mysql_query()\n");
        log_printf(LOG_ERROR, "mysql_error: %s\n", mysql_error(mysql));
        return -EIO;
    }
*/
    printf("fsck done!\n");
    return ret;

}
