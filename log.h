/*
  mysqlfs - MySQL Filesystem
  Copyright (C) 2006 Michal Ludvig <michal@logix.cz>
  $Id: log.h,v 1.2 2006/09/13 10:54:37 ludvigm Exp $

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/** @file */

/** FILE representing the open log file.  Defaulting to stderr, this file is defined by log_init() in main() when the "-ologfile=" option is used */
extern FILE *log_file;

/** bitfield of the log levels that are to be logged to the log_file.  Defaults to LOG_ERROR | LOG_INFO */
extern int log_types_mask;

/** defines logging-levels similar to syslog */
enum log_types {
  LOG_ERROR	= 0x0001,
  LOG_WARNING	= 0x0002,
  LOG_INFO	= 0x0004,
  LOG_DEBUG	= 0x0008,

  LOG_D_OTHER	= 0x0100 | LOG_DEBUG,
  LOG_D_SQL	= 0x0200 | LOG_DEBUG,
  LOG_D_CALL	= 0x0400 | LOG_DEBUG,
  LOG_D_POOL	= 0x0800 | LOG_DEBUG,
  
  LOG_MASK_MAJOR	= 0x000F,
  LOG_MASK_MINOR	= 0xFF00,
};

/** log a variable-format/token log message */
int log_printf(enum log_types type, const char *logmsg, ...);

/**
 * initialize the log.  If "stdout" or "stderr" are used, the existing streams will be returned.
 * @return a pointer to the newly-opened log file, or stdout or stderr if those strings are used
 * @param filename name of file to use
 * @param verbose print a message to show the filename being opened
 */
FILE *log_init(const char *filename, int verbose);

/** close the log file */
void log_finish(FILE *f);
