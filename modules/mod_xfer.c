/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 1997, 1998 Public Flood Software
 * Copyright (c) 1999, 2000 MacGyver aka Habeeb J. Dihu <macgyver@tos.net>
 * Copyright (c) 2001, 2002, 2003 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, Public Flood Software/MacGyver aka Habeeb J. Dihu
 * and other respective copyright holders give permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 */

/* Data transfer module for ProFTPD
 *
 * $Id: mod_xfer.c,v 1.122 2003-02-12 19:03:35 castaglia Exp $
 */

#include "conf.h"
#include "privs.h"

#include <signal.h>

#ifdef HAVE_SYS_SENDFILE_H
#include <sys/sendfile.h>
#endif

#ifdef HAVE_REGEX_H
#include <regex.h>
#endif

extern module auth_module;
extern pid_t mpid;

/* From the auth module */
char *auth_map_uid(int);
char *auth_map_gid(int);

void xfer_abort(pr_netio_stream_t *, int);

/* Variables for this module */
static pr_fh_t *retr_fh = NULL;
static pr_fh_t *stor_fh = NULL;

/* Transfer rate variables */
static long double xfer_rate_kbps = 0.0, xfer_rate_bps = 0.0;
static off_t xfer_rate_freebytes = 0.0;
static unsigned char have_xfer_rate = FALSE;

/* Transfer rate functions */
static void xfer_rate_lookup(cmd_rec *);
static unsigned char xfer_rate_parse_cmdlist(config_rec *, char *);
static void xfer_rate_sigmask(unsigned char);
static void xfer_rate_throttle(off_t);

module xfer_module;

static int xfer_errno;

static unsigned long find_max_nbytes(char *directive) {
  config_rec *c = NULL;
  unsigned int ctxt_precedence = 0;
  unsigned char have_user_limit, have_group_limit, have_class_limit,
    have_all_limit;
  unsigned long max_nbytes = 0UL;

  have_user_limit = have_group_limit = have_class_limit =
    have_all_limit = FALSE;

  c = find_config(CURRENT_CONF, CONF_PARAM, directive, FALSE);

  while (c) {
    if (c->argc == 3) {
      if (!strcmp(c->argv[1], "user")) {

        if (pr_user_or_expression((char **) &c->argv[2])) {
          if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

            /* Set the context precedence */
            ctxt_precedence = *((unsigned int *) c->argv[1]);

            max_nbytes = *((unsigned long *) c->argv[0]);

            have_group_limit = have_class_limit = have_all_limit = FALSE;
            have_user_limit = TRUE;
          }
        }

      } else if (!strcmp(c->argv[1], "group")) {

        if (pr_group_or_expression((char **) &c->argv[2])) {
          if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

            /* Set the context precedence */
            ctxt_precedence = *((unsigned int *) c->argv[1]);

            max_nbytes = *((unsigned long *) c->argv[0]);

            have_user_limit = have_class_limit = have_all_limit = FALSE;
            have_group_limit = TRUE;
          }
        }

      } else if (!strcmp(c->argv[1], "class")) {

        if (pr_class_or_expression((char **) &c->argv[2])) {
          if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

            /* Set the context precedence */
            ctxt_precedence = *((unsigned int *) c->argv[1]);

            max_nbytes = *((unsigned long *) c->argv[0]);

            have_user_limit = have_group_limit = have_all_limit = FALSE;
            have_class_limit = TRUE;
          }
        }
      }

    } else {

      if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

        /* Set the context precedence. */
        ctxt_precedence = *((unsigned int *) c->argv[1]);

        max_nbytes = *((unsigned long *) c->argv[0]);

        have_user_limit = have_group_limit = have_class_limit = FALSE;
        have_all_limit = TRUE;
      }
    }

    c = find_config_next(c, c->next, CONF_PARAM, directive, FALSE);
  }

  /* Print out some nice debugging information. */
  if (max_nbytes > 0UL &&
      (have_user_limit || have_group_limit ||
       have_class_limit || have_all_limit)) {
    log_debug(DEBUG5, "%s (%lu bytes) in effect for %s",
      directive, max_nbytes,
      have_user_limit ? "user " : have_group_limit ? "group " :
      have_class_limit ? "class " : "all");
  }

  return max_nbytes;
}

static unsigned long parse_max_nbytes(char *nbytes_str, char *units_str) {
  long res;
  unsigned long nbytes;
  char *endp = NULL;
  float units_factor = 0.0;

  /* clear any previous local errors */
  xfer_errno = 0;

  /* first, check the given units to determine the correct mulitplier
   */
  if (!strcasecmp("Gb", units_str)) {
    units_factor = 1024.0 * 1024.0 * 1024.0;

  } else if (!strcasecmp("Mb", units_str)) {
    units_factor = 1024.0 * 1024.0;

  } else if (!strcasecmp("Kb", units_str)) {
    units_factor = 1024.0;

  } else if (!strcasecmp("b", units_str)) {
    units_factor = 1.0;

  } else {
    xfer_errno = EINVAL;
    return 0;
  }

  /* make sure a number was given */
  if (!isdigit((int) *nbytes_str)) {
    xfer_errno = EINVAL;
    return 0;
  }

  /* knowing the factor, now convert the given number string to a real
   * number
   */
  res = strtol(nbytes_str, &endp, 10);

  if (errno == ERANGE) {
    xfer_errno = ERANGE;
    return 0;
  }

  if (endp && *endp) {
    xfer_errno = EINVAL;
    return 0;
  }

  /* don't bother to apply the factor if that will cause the number to
   * overflow
   */
  if (res > (ULONG_MAX / units_factor)) {
    xfer_errno = ERANGE;
    return 0;
  }

  nbytes = (unsigned long) res * units_factor;
  return nbytes;
}

static void _log_transfer(char direction, char abort_flag) {
  struct timeval end_time;
  char *fullpath = NULL;

  memset(&end_time, '\0', sizeof(end_time));

  if (session.xfer.start_time.tv_sec != 0) {
    gettimeofday(&end_time, NULL);
    end_time.tv_sec -= session.xfer.start_time.tv_sec;

    if (end_time.tv_usec >= session.xfer.start_time.tv_usec)
      end_time.tv_usec -= session.xfer.start_time.tv_usec;

    else {
      end_time.tv_usec = 1000000L - (session.xfer.start_time.tv_usec -
        end_time.tv_usec);
      end_time.tv_sec--;
    }
  }

  fullpath = dir_abs_path(session.xfer.p, session.xfer.path, TRUE);

  if ((session.sf_flags & SF_ANON) != 0) {
    log_xfer(end_time.tv_sec, session.c->remote_name, session.xfer.total_bytes,
      fullpath, (session.sf_flags & SF_ASCII ? 'a' : 'b'), direction,
      'a', session.anon_user, abort_flag);

  } else {
    log_xfer(end_time.tv_sec, session.c->remote_name, session.xfer.total_bytes,
      fullpath, (session.sf_flags & SF_ASCII ? 'a' : 'b'), direction,
      'r', session.user, abort_flag);
  }

  log_debug(DEBUG1, "Transfer %s %" PR_LU " bytes in %ld.%02lu seconds",
    abort_flag == 'c' ? "completed:" : "aborted after",
    session.xfer.total_bytes, (long) end_time.tv_sec,
    (unsigned long)(end_time.tv_usec / 10000));
}

/* Code borrowed from src/dirtree.c's get_word() -- modified to separate
 * words on commas as well as spaces.
 */
static char *get_cmd_from_list(char **list) {
  char *res = NULL, *dst = NULL;
  unsigned char quote_mode = FALSE;

  while (**list && isspace((int) **list))
    (*list)++;

  if (!**list)
    return NULL;

  res = dst = *list;

  if (**list == '\"') {
    quote_mode = TRUE;
    (*list)++;
  }

  while (**list && **list != ',' &&
      (quote_mode ? (**list != '\"') : (!isspace((int) **list)))) {

    if (**list == '\\' && quote_mode) {

      /* escaped char */
      if (*((*list) + 1))
        *dst = *(++(*list));
    }

    *dst++ = **list;
    ++(*list);
  }

  if (**list)
    (*list)++;

  *dst = '\0';

  return res;
}

static void xfer_rate_lookup(cmd_rec *cmd) {
  config_rec *c = NULL;
  char *xfer_cmd = NULL;
  unsigned char have_user_rate = FALSE, have_group_rate = FALSE,
    have_class_rate = FALSE, have_all_rate = FALSE;
  unsigned int precedence = 0;

  /* Do nothing if values are already cached. */
  if (have_xfer_rate)
    return;

  /* Make sure the variables are (re)initialized */
  xfer_rate_kbps = xfer_rate_bps = 0.0;
  xfer_rate_freebytes = 0;
  have_xfer_rate = FALSE;

  c = find_config(CURRENT_CONF, CONF_PARAM, "TransferRate", FALSE);

  /* Note: need to cycle through all the matching config_recs, and using
   * the information from the current config_rec only if it matches
   * the target *and* has a higher precedence than any of the previously
   * found config_recs.
   */
  while (c) {
    char **cmdlist = (char **) c->argv[0];
    unsigned char matched_cmd = FALSE;

    /* Does this TransferRate apply to the current command?  Note: this
     * could be made more efficient by using bitmasks rather than string
     * comparisons.
     */
    for (xfer_cmd = *cmdlist; xfer_cmd; xfer_cmd = *(cmdlist++)) {
      if (!strcasecmp(xfer_cmd, cmd->argv[0])) {
        matched_cmd = TRUE;
        break;
      }
    }

    /* No -- continue on to the next TransferRate. */
    if (!matched_cmd) {
      c = find_config_next(c, c->next, CONF_PARAM, "TransferRate", FALSE);
      continue;
    }

    if (c->argc > 4) {
      if (!strcmp(c->argv[4], "user")) {

        if (pr_user_or_expression((char **) &c->argv[5]) &&
            *((unsigned int *) c->argv[3]) > precedence) {

          /* Set the precedence. */
          precedence = *((unsigned int *) c->argv[3]);

          xfer_rate_kbps = *((long double *) c->argv[1]);
          xfer_rate_freebytes = *((off_t *) c->argv[2]);
          have_xfer_rate = TRUE;
          have_user_rate = TRUE;
          have_group_rate = have_class_rate = have_all_rate = FALSE;
        }

      } else if (!strcmp(c->argv[4], "group")) {

        if (pr_group_and_expression((char **) &c->argv[5]) &&
            *((unsigned int *) c->argv[3]) > precedence) {

          /* Set the precedence. */
          precedence = *((unsigned int *) c->argv[3]);

          xfer_rate_kbps = *((long double *) c->argv[1]);
          xfer_rate_freebytes = *((off_t *) c->argv[2]);
          have_xfer_rate = TRUE;
          have_group_rate = TRUE;
          have_user_rate = have_class_rate = have_all_rate = FALSE;
        }

      } else if (!strcmp(c->argv[4], "class")) {

        if (pr_class_or_expression((char **) &c->argv[5]) &&
          *((unsigned int *) c->argv[3]) > precedence) {

          /* Set the precedence. */
          precedence = *((unsigned int *) c->argv[3]);

          xfer_rate_kbps = *((long double *) c->argv[1]);
          xfer_rate_freebytes = *((off_t *) c->argv[2]);
          have_xfer_rate = TRUE;
          have_class_rate = TRUE;
          have_user_rate = have_group_rate = have_all_rate = FALSE;
        }
      }

    } else {

      if (*((unsigned int *) c->argv[3]) > precedence) {

        /* Set the precedence. */
        precedence = *((unsigned int *) c->argv[3]);

        xfer_rate_kbps = *((long double *) c->argv[1]);
        xfer_rate_freebytes = *((off_t *) c->argv[2]);
        have_xfer_rate = TRUE;
        have_all_rate = TRUE;
        have_user_rate = have_group_rate = have_class_rate = FALSE;
      }
    }

    c = find_config_next(c, c->next, CONF_PARAM, "TransferRate", FALSE);
  }

  /* Print out a helpful debugging message. */
  if (have_xfer_rate) {
    log_debug(DEBUG3, "TransferRate (%.3Lf KB/s, %" PR_LU
        " bytes free) in effect%s", xfer_rate_kbps, xfer_rate_freebytes,
      have_user_rate ? " for current user" :
      have_group_rate ? " for current group" :
      have_class_rate ? " for current class" : "");

    /* Convert the configured Kbps to bytes per usec, for use later.
     * The 1024.0 factor converts for Kbytes to bytes, and the
     * 1000000.0 factor converts from secs to usecs.
     */
    xfer_rate_bps = xfer_rate_kbps * 1024.0;
  }
}

static unsigned char xfer_rate_parse_cmdlist(config_rec *c, char *cmdlist) {
  char *cmd = NULL;
  array_header *cmds = NULL;

  /* Allocate an array_header. */
  cmds = make_array(c->pool, 0, sizeof(char *));

  /* Add each command to the array, checking for invalid commands or
   * duplicates.
   */
  while ((cmd = get_cmd_from_list(&cmdlist)) != NULL) {

    /* Is the given command a valid one for this directive? */
    if (strcasecmp(cmd, C_APPE) && strcasecmp(cmd, C_RETR) &&
        strcasecmp(cmd, C_STOR) && strcasecmp(cmd, C_STOU)) {
      log_debug(DEBUG0, "invalid TransferRate command: %s", cmd);
      return FALSE;
    }

    *((char **) push_array(cmds)) = pstrdup(c->pool, cmd);
  }

  /* Terminate the array with a NULL. */
  *((char **) push_array(cmds)) = NULL;

  /* Store the array of commands in the config_rec. */
  c->argv[0] = (void *) cmds->elts;

  return TRUE;
}

/* Very similar to the {block,unblock}_signals() function, this masks most
 * of the same signals -- except for TERM.  This allows a throttling process
 * to be killed by the admin.
 */
static void xfer_rate_sigmask(unsigned char block) {
  static sigset_t sig_set;

  if (block) {
    sigemptyset(&sig_set);

    sigaddset(&sig_set, SIGCHLD);
    sigaddset(&sig_set, SIGUSR1);
    sigaddset(&sig_set, SIGINT);
    sigaddset(&sig_set, SIGQUIT);
    sigaddset(&sig_set, SIGALRM);
#ifdef SIGIO
    sigaddset(&sig_set, SIGIO);
#endif /* SIGIO */
#ifdef SIGBUS
    sigaddset(&sig_set, SIGBUS);
#endif /* SIGBUS */
    sigaddset(&sig_set, SIGHUP);

    sigprocmask(SIG_BLOCK, &sig_set, NULL);

  } else
    sigprocmask(SIG_UNBLOCK, &sig_set, NULL);
}

/* Returns the difference, in secs, between the given timeval and now. */
static long xfer_rate_since(struct timeval *then) {
  struct timeval now;
  gettimeofday(&now, NULL);

  return ((now.tv_sec - then->tv_sec) * 1000000L +
    (now.tv_usec - then->tv_usec));
}

static void xfer_rate_throttle(off_t xferlen) {
  long ideal = 0.0, elapsed = 0.0;

  /* Calculate the time interval since the transfer of data started. */
  elapsed = xfer_rate_since(&session.xfer.start_time);

  /* Perform no throttling if no throttling has been configured. */
  if (!have_xfer_rate) {

    /* Update the scoreboard. */
    pr_scoreboard_update_entry(getpid(),
      PR_SCORE_XFER_LEN, xferlen,
      PR_SCORE_XFER_ELAPSED, (unsigned long) elapsed,
      NULL);

    return;
  }

  /* Give credit for any configured freebytes. */
  if (xferlen && xfer_rate_freebytes) {

    if (xfer_rate_freebytes >= xferlen) {
       /* Decrement the number of freebytes by the total number of bytes
        * sent.  Since there are still more freebytes to be used, just
        * return now, after updating the timeval struc.
        */
       xfer_rate_freebytes -= xferlen;

      /* Update the scoreboard. */
      pr_scoreboard_update_entry(getpid(),
        PR_SCORE_XFER_LEN, xferlen,
        PR_SCORE_XFER_ELAPSED, (unsigned long) elapsed,
        NULL);

       return;

    } else {
      xferlen -= xfer_rate_freebytes;

      /* Make sure that, the next time through, the freebytes are not
       * credited again.
       */
      xfer_rate_freebytes = 0;
    }
  }

  ideal = xferlen * 1000000.0 / xfer_rate_bps;

  if (ideal > elapsed) {
    struct timeval tv;

    /* Setup for the select.  We use select() instead of usleep() because it
     * seems to be far more portable across platforms.
     */
    tv.tv_usec = ideal - elapsed;
    tv.tv_sec = tv.tv_usec / 1000000L;
    tv.tv_usec = tv.tv_usec % 1000000L;

    log_debug(DEBUG7, "transferring too fast, delaying %ld sec%s, %ld usecs",
      (long int) tv.tv_sec, tv.tv_sec == 1 ? "" : "s", (long int) tv.tv_usec);

    /* No interruptions, please... */
    xfer_rate_sigmask(TRUE);

    if (select(0, NULL, NULL, NULL, &tv) < 0)
      log_pri(LOG_WARNING, "warning: unable to throttle bandwidth: %s",
        strerror(errno));

    xfer_rate_sigmask(FALSE);
    pr_signals_handle();

    /* Update the scoreboard. */
    pr_scoreboard_update_entry(getpid(),
      PR_SCORE_XFER_LEN, xferlen,
      PR_SCORE_XFER_ELAPSED, (unsigned long) ideal,
      NULL);

  } else {

    /* Update the scoreboard. */
    pr_scoreboard_update_entry(getpid(),
      PR_SCORE_XFER_LEN, xferlen,
      PR_SCORE_XFER_ELAPSED, (unsigned long) elapsed,
      NULL);
  }

  return;
}

static int _transmit_normal(char *buf, long bufsize) {
  long count;

  if ((count = pr_fsio_read(retr_fh, buf, bufsize)) <= 0)
    return 0;

  return pr_data_xfer(buf, count);
}

#ifdef HAVE_SENDFILE
static int _transmit_sendfile(off_t count, off_t *offset,
    pr_sendfile_t *retval) {

  /* We don't use sendfile() if:
   * - We're using bandwidth throttling.
   * - We're transmitting an ASCII file.
   * - There's no data left to transmit.
   */
  if (have_xfer_rate ||
     !(session.xfer.file_size - count) ||
     (session.sf_flags & (SF_ASCII | SF_ASCII_OVERRIDE))) {
    return 0;
  }

 retry:
  *retval = pr_data_sendfile(PR_FH_FD(retr_fh), offset,
    session.xfer.file_size - count);

  if (*retval == -1) {
    switch (errno) {
    case EAGAIN:
    case EINTR:
      /* Interrupted call, or the other side wasn't ready yet.
       */
      goto retry;

    case EPIPE:
    case ECONNRESET:
    case ETIMEDOUT:
    case EHOSTUNREACH:
      /* Other side broke the connection.
       */
      break;

#ifdef ENOSYS
    case ENOSYS:
#endif /* ENOSYS */

    case EINVAL:
      /* No sendfile support, apparently.  Try it the normal way.
       */
      return 0;
      break;

    default:
      log_pri(PR_LOG_ERR,
              "_transmit_sendfile error "
              "(reverting to normal data transmission) %d: %s.",
              errno, strerror(errno));
      return 0;
    }
  }

  return 1;
}
#endif /* HAVE_SENDFILE */

static long _transmit_data(off_t count, off_t offset, char *buf, long bufsize) {
#ifdef HAVE_SENDFILE
  pr_sendfile_t retval;

  if (!_transmit_sendfile(count, &offset, &retval))
    return _transmit_normal(buf, bufsize);
  else
    return (long) retval;
#else
  return _transmit_normal(buf, bufsize);
#endif /* HAVE_SENDFILE */
}

static void _stor_chown(void) {
  struct stat sbuf;
  char *xfer_path = NULL;

  if (session.xfer.xfer_type == STOR_HIDDEN)
    xfer_path = session.xfer.path_hidden;
  else
    xfer_path = session.xfer.path;

  /* session.fsgid defaults to -1, so chown(2) won't chgrp unless specifically
   * requested via GroupOwner.
   */
  if ((session.fsuid != (uid_t) -1) && xfer_path) {
    int err = 0, iserr = 0;

    PRIVS_ROOT
    if (pr_fsio_chown(xfer_path, session.fsuid, session.fsgid) == -1) {
      iserr++;
      err = errno;
    }
    PRIVS_RELINQUISH

    if (iserr)
      log_pri(PR_LOG_WARNING, "chown(%s) as root failed: %s", xfer_path,
        strerror(err));

    else {

      if (session.fsgid != (gid_t) -1)
        log_debug(DEBUG2, "root chown(%s) to uid %lu, gid %lu successful",
          xfer_path, (unsigned long) session.fsuid,
          (unsigned long) session.fsgid);

      else
        log_debug(DEBUG2, "root chown(%s) to uid %lu successful", xfer_path,
          (unsigned long) session.fsuid);

      pr_fs_clear_cache();
      pr_fsio_stat(xfer_path, &sbuf);

      if (pr_fsio_chmod(xfer_path, sbuf.st_mode) < 0)
        log_debug(DEBUG0, "chmod(%s) to %04o failed: %s", xfer_path,
          (unsigned int) sbuf.st_mode, strerror(errno));
    }

  } else if ((session.fsgid != (gid_t) -1) && xfer_path) {

    if (pr_fsio_chown(xfer_path, (uid_t) -1, session.fsgid) == -1)
      log_pri(PR_LOG_WARNING, "chown(%s) failed: %s", xfer_path,
        strerror(errno));

    else {

      log_debug(DEBUG2, "chown(%s) to gid %lu successful", xfer_path,
        (unsigned long) session.fsgid);

      pr_fs_clear_cache();
      pr_fsio_stat(xfer_path, &sbuf);

      if (pr_fsio_chmod(xfer_path, sbuf.st_mode) < 0)
        log_debug(DEBUG0, "chmod(%s) to %04o failed: %s", xfer_path,
          (unsigned int) sbuf.st_mode, strerror(errno));
    }
  }
}

static void retr_abort(void) {
  /* Isn't necessary to send anything here, just cleanup */

  if (retr_fh) {
    pr_fsio_close(retr_fh);
    retr_fh = NULL;
  }

  _log_transfer('o', 'i');
}

static void retr_complete(void) {
  pr_fsio_close(retr_fh);
  retr_fh = NULL;
}

static void stor_abort(void) {
  unsigned char *delete_stores = NULL;

  if (stor_fh) {
    pr_fsio_close(stor_fh);
    stor_fh = NULL;
  }

  if (session.xfer.xfer_type == STOR_HIDDEN) {
    /* If hidden stor aborted, remove only hidden file, not real one */
    if (session.xfer.path_hidden)
      pr_fsio_unlink(session.xfer.path_hidden);

  } else if (session.xfer.path) {
    if ((delete_stores = get_param_ptr(CURRENT_CONF, "DeleteAbortedStores",
        FALSE)) != NULL && *delete_stores == TRUE)
      pr_fsio_unlink(session.xfer.path);
  }

  _log_transfer('i', 'i');
}

static void stor_complete(void) {
  pr_fsio_close(stor_fh);
  stor_fh = NULL;
}

/* Exit handler, call abort functions if a transfer is in progress. */
static void xfer_exit_cb(void) {
  if (session.sf_flags & SF_XFER) {

    if (session.xfer.direction == PR_NETIO_IO_RD)
       /* An upload is occurring... */
      stor_abort();

    else
      /* A download is occurring... */
      retr_abort();
  }
}

/* This function clears any cached TransferRate values, as TransferRates
 * may be set on a directory-by-directory basis.
 */
MODRET xfer_reset_rate(cmd_rec *cmd) {
  have_xfer_rate = FALSE;
  return DECLINED(cmd);
}

/* This is a PRE_CMD handler that checks security, etc, and places the full
 * filename to receive in cmd->private [note that we CANNOT use cmd->tmp_pool
 * for this, as tmp_pool only lasts for the duration of this function.
 */

MODRET xfer_pre_stor(cmd_rec *cmd) {
  char *dir;
  mode_t fmode;
  privdata_t *p, *p_hidden;
  unsigned char *hidden_stores = NULL, *allow_overwrite = NULL,
    *allow_restart = NULL;

  if (cmd->argc < 2) {
    pr_response_add_err(R_500, "'%s' not understood", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  dir = dir_best_path(cmd->tmp_pool, cmd->arg);

  if (!dir || !dir_check(cmd->tmp_pool, cmd->argv[0], cmd->group, dir, NULL)) {
    pr_response_add_err(R_550, "%s: %s", cmd->arg, strerror(errno));
    return ERROR(cmd);
  }

  fmode = file_mode(dir);

  allow_overwrite = get_param_ptr(CURRENT_CONF, "AllowOverwrite", FALSE);

  if (fmode && (session.xfer.xfer_type != STOR_APPEND) &&
      (!allow_overwrite || *allow_overwrite == FALSE)) {
    log_debug(DEBUG6, "AllowOverwrite denied permission for %s", cmd->arg);
    pr_response_add_err(R_550, "%s: Overwrite permission denied", cmd->arg);
    return ERROR(cmd);
  }

  if (fmode && !S_ISREG(fmode)) {
    pr_response_add_err(R_550, "%s: Not a regular file", cmd->arg);
    return ERROR(cmd);
  }

  /* If restarting, check permissions on this directory, if
   * AllowStoreRestart is set, permit it
   */
  allow_restart = get_param_ptr(CURRENT_CONF, "AllowStoreRestart", FALSE);

  if (fmode &&
     (session.restart_pos || (session.xfer.xfer_type == STOR_APPEND)) &&
     (!allow_restart || *allow_restart == FALSE)) {

    pr_response_add_err(R_451, "%s: Append/Restart not permitted, try again",
      cmd->arg);
    session.restart_pos = 0L;
    session.xfer.xfer_type = STOR_DEFAULT;
    return ERROR(cmd);
  }

  /* Otherwise everthing is good */
  p = mod_privdata_alloc(cmd, "stor_filename", strlen(dir) + 1);
  sstrncpy(p->value.str_val, dir, strlen(dir) + 1);

  if ((hidden_stores = get_param_ptr(CURRENT_CONF, "HiddenStores",
      FALSE)) != NULL && *hidden_stores == TRUE) {

    /* We have to also figure out the temporary hidden file name for receiving
     * this transfer.  Length is +5 due to .in. prepended and "." at end.
     */
    char *c = NULL;
    int dotcount, foundslash, basenamestart, maxlen;

    dotcount = foundslash = basenamestart = 0;

    /* Figure out where the basename starts */
    for (c=dir; *c; ++c) {
      if (*c == '/') {
        foundslash = 1;
        basenamestart = dotcount = 0;
      } else if (*c == '.') {
        ++ dotcount;

        /* Keep track of leading dots, ... is normal, . and .. are special.
         * So if we exceed ".." it becomes a normal file, retroactively consider
         * this the possible start of the basename
         */
        if ((dotcount > 2) && (!basenamestart))
          basenamestart = ((unsigned long)c - (unsigned long)dir) - dotcount;
      } else {
        /* We found a nonslash, nondot character; if this is the first time
         * we found one since the last slash, remember this as the possible
         * start of the basename.
         */
        if (!basenamestart)
          basenamestart = ((unsigned long)c - (unsigned long)dir) - dotcount;
      }
    }

    if (!basenamestart) {
      /* This probably shouldn't happen */
      pr_response_add_err(R_451, "%s: Bad file name", dir);
      return ERROR(cmd);
    }

    maxlen = strlen(dir) + 1 + 5;

    if (maxlen > MAXPATHLEN) {
      /* This probably shouldn't happen */
      pr_response_add_err(R_451, "%s: File name too long", dir);
      return ERROR(cmd);
    }

    p_hidden = mod_privdata_alloc(cmd, "stor_hidden_filename", maxlen);

    if (! foundslash) {
      /* Simple local file name */
      sstrncpy(p_hidden->value.str_val, ".in.", maxlen);
      sstrcat(p_hidden->value.str_val, dir, maxlen);
      sstrcat(p_hidden->value.str_val, ".", maxlen);
      log_pri(PR_LOG_DEBUG, "Local path, will rename %s to %s.",
        p_hidden->value.str_val, p->value.str_val);
    } else {
      /* Complex relative path or absolute path */
      sstrncpy(p_hidden->value.str_val, dir, maxlen);
      p_hidden->value.str_val[basenamestart] = '\0';
      sstrcat(p_hidden->value.str_val, ".in.", maxlen);
      sstrcat(p_hidden->value.str_val, dir + basenamestart, maxlen);
      sstrcat(p_hidden->value.str_val, ".", maxlen);
      log_pri(PR_LOG_DEBUG, "Complex path, will rename %s to %s.",
        p_hidden->value.str_val, p->value.str_val);

      if (file_mode(p_hidden->value.str_val)) {
        pr_response_add_err(R_550,"%s: Temporary hidden file %s already exists",
                cmd->arg, p_hidden->value.str_val);
        return ERROR(cmd);
      }
    }

    session.xfer.xfer_type = STOR_HIDDEN;
  }

  return HANDLED(cmd);
}

/* xfer_pre_stou() is a PRE_CMD handler that changes the uploaded filename
 * to a unique one, after making the requisite security and authorization
 * checks.
 */
MODRET xfer_pre_stou(cmd_rec *cmd) {
  config_rec *c = NULL;
  privdata_t *priv = NULL;
  char *prefix = "ftp", *filename = NULL;
  int tmpfd;
  mode_t mode;
  unsigned char *allow_overwrite = NULL;

  /* Some FTP clients are "broken" in that they will send a filename
   * along with STOU.  Technically this violates RFC959, but for now, just
   * ignore that filename.  Stupid client implementors.
   */

  if (cmd->argc > 2) {
    pr_response_add_err(R_500, "'%s' not understood.", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  /* Watch for STOU preceded by REST, which makes no sense.
   *
   *   REST: session.restart_pos > 0
   */
  if (session.restart_pos) {
    pr_response_add_err(R_550, "STOU incompatible with REST");
    return ERROR(cmd);
  }

  /* Generate the filename to be stored, depending on the configured
   * unique filename prefix.
   */
  if ((c = find_config(CURRENT_CONF, CONF_PARAM, "StoreUniquePrefix",
      FALSE)) != NULL)
    prefix = c->argv[0];

  /* Now, construct the unique filename using the cmd_rec's pool, the
   * prefix, and mkstemp().
   */
  filename = pstrcat(cmd->pool, prefix, "XXXXXX", NULL);

  if ((tmpfd = mkstemp(filename)) < 0) {
    log_pri(PR_LOG_ERR, "error: unable to use mkstemp(): %s", strerror(errno));

    /* If we can't guarantee a unique filename, refuse the command. */
    pr_response_add_err(R_450, "%s: unable to generate unique filename",
      cmd->argv[0]);
    return ERROR(cmd);

  } else {
    cmd->arg = filename;

    /* Close the unique file.  This introduces a small race condition
     * between the time this function returns, and the STOU CMD handler
     * opens the unique file, but this may have to do, as closing that
     * race would involve some major restructuring.
     */
    close(tmpfd);
  }

  /* It's OK to reuse the char * pointer for filename.
   */
  filename = dir_best_path(cmd->tmp_pool, cmd->arg);

  if (!filename || !dir_check(cmd->tmp_pool, cmd->argv[0], cmd->group,
      filename, NULL)) {

    /* Do not forget to delete the file created by mkstemp(3) if there is
     * an error.
     */
    pr_fsio_unlink(cmd->arg);

    pr_response_add_err(R_550, "%s: %s", cmd->arg, strerror(errno));
    return ERROR(cmd);
  }

  mode = file_mode(filename);

  /* Note: this case should never happen: how one can be appending to
   * a supposedly unique filename?  Should probably be removed...
   */
  allow_overwrite = get_param_ptr(CURRENT_CONF, "AllowOverwrite", FALSE);

  if (mode && session.xfer.xfer_type != STOR_APPEND &&
      (!allow_overwrite || *allow_overwrite == FALSE)) {
    log_debug(DEBUG6, "AllowOverwrite denied permission for %s", cmd->arg);
    pr_response_add_err(R_550, "%s: Overwrite permission denied", cmd->arg);
    return ERROR(cmd);
  }

  /* Not likely to _not_ be a regular file, but just to be certain...
   */
  if (mode && !S_ISREG(mode)) {
    pr_fsio_unlink(cmd->arg);
    pr_response_add_err(R_550, "%s: Not a regular file", cmd->arg);
    return ERROR(cmd);
  }

  /* Otherwise everthing is good */
  priv = mod_privdata_alloc(cmd, "stor_filename", strlen(filename) + 1);
  sstrncpy(priv->value.str_val, filename, strlen(filename) + 1);

  session.xfer.xfer_type = STOR_UNIQUE;

  return HANDLED(cmd);
}

/* xfer_post_stou() is a POST_CMD handler that changes the mode of the
 * STOU file from 0600, which is what mkstemp() makes it, to 0666,
 * the default for files uploaded via STOR.  This is to prevent users
 * from being surprised.
 */
MODRET xfer_post_stou(cmd_rec *cmd) {

  /* This is the same mode as used in src/fs.c.  Should probably be
   * available as a macro.
   */
  mode_t mode = 0666;

  if (pr_fsio_chmod(cmd->arg, mode) < 0) {

    /* Not much to do but log the error. */
    log_pri(PR_LOG_ERR, "error: unable to chmod '%s': %s", cmd->arg,
      strerror(errno));
  }

  return HANDLED(cmd);
}

/* xfer_pre_appe() is the PRE_CMD handler for the APPE command, which
 * simply sets xfer_type to STOR_APPEND and calls xfer_pre_stor().
 */

MODRET xfer_pre_appe(cmd_rec *cmd) {
  session.xfer.xfer_type = STOR_APPEND;
  session.restart_pos = 0L;

  return xfer_pre_stor(cmd);
}

MODRET xfer_stor(cmd_rec *cmd) {
  char *dir;
  char *lbuf;
  int bufsize,len;
  off_t nbytes_stored, nbytes_max_store = 0;
  unsigned char have_limit = FALSE;
  struct stat sbuf;
  off_t respos = 0;
  privdata_t *p, *p_hidden;

#if defined(HAVE_REGEX_H) && defined(HAVE_REGCOMP)
  regex_t *preg;
  int ret;
#endif /* REGEX */

  /* this function sets static module variables for later throttling */
  xfer_rate_lookup(cmd);

  p_hidden = NULL;
  p = mod_privdata_find(cmd, "stor_filename", NULL);

  if (!p) {
    pr_response_add_err(R_550,"%s: internal error, stor_filename not set by cmd_pre_stor",cmd->arg);
    return ERROR(cmd);
  }

  dir = p->value.str_val;

  if (session.xfer.xfer_type == STOR_HIDDEN) {
    p_hidden = mod_privdata_find(cmd,"stor_hidden_filename",NULL);
    if (!p_hidden) {
      pr_response_add_err(R_550,"%s: internal error, stor_hidden_filename not set by cmd_pre_retr",cmd->arg);
      return ERROR(cmd);
    }
  }

#if defined(HAVE_REGEX_H) && defined(HAVE_REGCOMP)
  preg = (regex_t *) get_param_ptr(TOPLEVEL_CONF, "PathAllowFilter", FALSE);

  if (preg && ((ret = regexec(preg,cmd->arg, 0, NULL, 0)) != 0)) {
    char errmsg[200];
    regerror(ret, preg, errmsg, sizeof(errmsg));
    log_debug(DEBUG2, "'%s' denied by PathAllowFilter: %s", cmd->arg, errmsg);
    pr_response_add_err(R_550, "%s: Forbidden filename", cmd->arg);
    return ERROR(cmd);

  } else if (preg)
    log_debug(DEBUG8, "'%s' allowed by PathAllowFilter", cmd->arg);

  preg = (regex_t *) get_param_ptr(TOPLEVEL_CONF, "PathDenyFilter", FALSE);

  if (preg && ((ret = regexec(preg, cmd->arg, 0, NULL, 0)) == 0)) {
    char errmsg[200];
    regerror(ret, preg, errmsg, sizeof(errmsg));
    log_debug(DEBUG2, "'%s' denied by PathDenyFilter: %s", cmd->arg, errmsg);
    pr_response_add_err(R_550, "%s: Forbidden filename", cmd->arg);
    return ERROR(cmd);

  } else if (preg)
    log_debug(DEBUG8, "'%s' allowed by PathDenyFilter", cmd->arg);

#endif /* REGEX */

  if (session.xfer.xfer_type == STOR_HIDDEN)
    stor_fh = pr_fsio_open(p_hidden->value.str_val,
        O_WRONLY|(session.restart_pos ? 0 : O_CREAT|O_EXCL));

  else if (session.xfer.xfer_type == STOR_APPEND) {
    stor_fh = pr_fsio_open(dir, O_CREAT|O_WRONLY);

    if (stor_fh)
      if (pr_fsio_lseek(stor_fh, 0, SEEK_END) == (off_t) -1) {
        pr_fsio_close(stor_fh);
        stor_fh = NULL;
      }
  }

  else /* Normal session */
    stor_fh = pr_fsio_open(dir,
        O_WRONLY|(session.restart_pos ? 0 : O_TRUNC|O_CREAT));

  if (stor_fh && session.restart_pos) {
    int xerrno = 0;

    if (pr_fsio_lseek(stor_fh, session.restart_pos, SEEK_SET) == -1)
      xerrno = errno;

    else if (pr_fsio_stat(dir, &sbuf) == -1)
      xerrno = errno;

    if (xerrno) {
      pr_fsio_close(stor_fh);
      errno = xerrno;
      stor_fh = NULL;
    }

    /* Make sure that the requested offset is valid (within the size of the
     * file being resumed.
     */
    if (stor_fh && session.restart_pos > sbuf.st_size) {
      pr_response_add_err(R_554, "%s: invalid REST argument", cmd->arg);
      pr_fsio_close(stor_fh);
      stor_fh = NULL;
      return ERROR(cmd);
    }

    respos = session.restart_pos;
    session.restart_pos = 0L;
  }

  if (!stor_fh) {
    log_debug(DEBUG4, "unable to open '%s' for writing: %s", cmd->arg,
      strerror(errno));
    pr_response_add_err(R_550, "%s: %s", cmd->arg, strerror(errno));
    return ERROR(cmd);
  }

  /* Perform the actual transfer now */
  pr_data_init(cmd->arg, PR_NETIO_IO_RD);

  session.xfer.path = dir;

  if (session.xfer.xfer_type == STOR_HIDDEN)
    session.xfer.path_hidden = pstrdup(session.xfer.p,
      p_hidden->value.str_val);
  else
    session.xfer.path_hidden = NULL;

  session.xfer.file_size = respos;

  /* First, make sure the uploaded file has the requested ownership. */
  _stor_chown();

  if (pr_data_open(cmd->arg, NULL, PR_NETIO_IO_RD, 0) < 0) {
    stor_abort();
    pr_data_abort(0, TRUE);
    return HANDLED(cmd);
  }

  /* Initialize the number of bytes stored */
  nbytes_stored = 0;

  /* Retrieve the number of bytes to store, maximum, if present.
   * This check is needed during the pr_data_xfer() loop, below, because
   * the size of the file being uploaded isn't known in advance
   */
  if ((nbytes_max_store = find_max_nbytes("MaxStoreFileSize")) == 0UL)
    have_limit = FALSE;
  else
    have_limit = TRUE;

  /* Check the MaxStoreFileSize, and abort now if zero. */
  if (have_limit && nbytes_max_store == 0) {

    log_pri(PR_LOG_INFO, "MaxStoreFileSize (%" PR_LU " byte%s) reached: "
      "aborting transfer of '%s'", nbytes_max_store,
      nbytes_max_store != 1 ? "s" : "", dir);

    /* Abort the transfer. */
    stor_abort();

    /* Set errno to EPERM ("Operation not permitted") */
    pr_data_abort(EPERM, FALSE);
    return ERROR(cmd);
  }

  bufsize = (main_server->tcp_rwin > 0 ?
    main_server->tcp_rwin : PR_TUNABLE_BUFFER_SIZE);
  lbuf = (char*) palloc(cmd->tmp_pool, bufsize);

  while ((len = pr_data_xfer(lbuf, bufsize)) > 0) {
    if (XFER_ABORTED)
      break;

    nbytes_stored += len;

    /* Double-check the current number of bytes stored against the
     * MaxStoreFileSize, if configured.
     */
    if (have_limit && nbytes_stored > nbytes_max_store) {

      log_pri(PR_LOG_INFO, "MaxStoreFileSize (%" PR_LU " bytes) reached: "
        "aborting transfer of '%s'", nbytes_max_store, dir);

      /* Unlink the file being written. */
      pr_fsio_unlink(dir);

      /* Abort the transfer. */
      stor_abort();

      /* Set errno to EPERM ("Operation not permitted"). */
      pr_data_abort(EPERM, FALSE);
      return ERROR(cmd);
    }

    if ((len = pr_fsio_write(stor_fh, lbuf, len)) < 0) {
      int s_errno = errno;
      stor_abort();
      pr_data_abort(s_errno, FALSE);
      return ERROR(cmd);
    }

    /* If no throttling is configured, this does nothing. */
    xfer_rate_throttle(len);
  }

  if (XFER_ABORTED) {
    stor_abort();
    pr_data_abort(0, 0);
    return ERROR(cmd);

  } else if (len < 0) {
    stor_abort();
    pr_data_abort(PR_NETIO_ERRNO(session.d->instrm), FALSE);
    return ERROR(cmd);

  } else {

    /* If no throttling is configured, this does nothing. */
    xfer_rate_throttle(len);

    stor_complete();

    if (session.xfer.path && session.xfer.path_hidden) {

      if (pr_fsio_rename(session.xfer.path_hidden, session.xfer.path) != 0) {

        /* This should only fail on a race condition with a chmod/chown
         * or if STOR_APPEND is on and the permissions are squirrely.
         * The poor user will have to re-upload, but we've got more important
         * problems to worry about and this failure should be fairly rare.
         */
        log_pri(PR_LOG_WARNING, "Rename of %s to %s failed: %s.",
          session.xfer.path_hidden, session.xfer.path, strerror(errno));

        pr_response_add_err(R_550,"%s: rename of hidden file %s failed: %s",
          session.xfer.path, session.xfer.path_hidden, strerror(errno));

        pr_fsio_unlink(session.xfer.path_hidden);

        return ERROR(cmd);
      }
    }
    pr_data_close(FALSE);
  }

  return HANDLED(cmd);
}

MODRET xfer_rest(cmd_rec *cmd) {
  long int pos;
  char *endp = NULL;
  unsigned char *hidden_stores = NULL;

  if (cmd->argc != 2) {
    pr_response_add_err(R_500, "'%s': command not understood",
      get_full_cmd(cmd));
    return ERROR(cmd);
  }

  /* If we're using HiddenStores, then REST won't work. */
  if ((hidden_stores = get_param_ptr(CURRENT_CONF, "HiddenStores",
      FALSE)) != NULL && *hidden_stores == TRUE) {
    pr_response_add_err(R_501, "REST not compatible with server configuration");
    return ERROR(cmd);
  }

  pos = strtol(cmd->argv[1],&endp,10);

  if ((endp && *endp) || pos < 0) {
    pr_response_add_err(R_501, "REST requires a value greater than or equal to 0");
    return ERROR(cmd);
  }

  session.restart_pos = pos;

  pr_response_add(R_350, "Restarting at %ld. Send STORE or RETRIEVE to "
    "initiate transfer", pos);
  return HANDLED(cmd);
}

/* This is a PRE_CMD handler that checks security, etc, and places the full
 * filename to send in cmd->private [note that we CANNOT use cmd->tmp_pool
 * for this, as tmp_pool only lasts for the duration of this function.
 */
MODRET xfer_pre_retr(cmd_rec *cmd) {
  char *dir = NULL;
  mode_t fmode;
  privdata_t *p = NULL;
  unsigned char *allow_restart = NULL;

  if (cmd->argc < 2) {
    pr_response_add_err(R_500, "'%s' not understood", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  dir = dir_realpath(cmd->tmp_pool,cmd->arg);

  if (!dir || !dir_check(cmd->tmp_pool,cmd->argv[0],cmd->group,dir,NULL)) {
    pr_response_add_err(R_550, "%s: %s", cmd->arg, strerror(errno));
    return ERROR(cmd);
  }

  fmode = file_mode(dir);

  if (!S_ISREG(fmode)) {
    if (!fmode)
      pr_response_add_err(R_550, "%s: %s", cmd->arg, strerror(errno));
    else
      pr_response_add_err(R_550, "%s: Not a regular file", cmd->arg);
    return ERROR(cmd);
  }

  /* If restart is on, check to see if AllowRestartRetrieve is off, in
   * which case we disallow the transfer and clear restart_pos.
   */
  allow_restart = get_param_ptr(CURRENT_CONF, "AllowRetrieveRestart", FALSE);

  if (session.restart_pos &&
     (allow_restart && *allow_restart == FALSE)) {
    pr_response_add_err(R_451, "%s: Restart not permitted, try again",
      cmd->arg);
    session.restart_pos = 0L;
    return ERROR(cmd);
  }

  /* Otherwise everthing is good */
  p = mod_privdata_alloc(cmd, "retr_filename", strlen(dir)+1);
  sstrncpy(p->value.str_val, dir, strlen(dir) + 1);
  return HANDLED(cmd);
}

MODRET xfer_retr(cmd_rec *cmd) {
  char *dir = NULL, *lbuf;
  struct stat sbuf;
  off_t nbytes_max_retrieve = 0;
  unsigned char have_limit = FALSE;
  privdata_t *p;
  long bufsize, len = 0;
  off_t respos = 0, cnt = 0, cnt_steps = 0, cnt_next = 0;

  /* This function sets static module variables for later potential
   * throttling of the transfer.
   */
  xfer_rate_lookup(cmd);

  p = mod_privdata_find(cmd, "retr_filename", NULL);

  if (!p) {
    pr_response_add_err(R_550, "%s: internal error, what happened to "
      "cmd_pre_retr?!?", cmd->arg);
    return ERROR(cmd);
  }

  dir = p->value.str_val;

  if ((retr_fh = pr_fsio_open(dir, O_RDONLY)) == NULL) {
    /* Error opening the file. */
    pr_response_add_err(R_550, "%s: %s", cmd->arg, strerror(errno));
    return ERROR(cmd);
  }

  if (pr_fsio_stat(dir, &sbuf) < 0) {
    /* Error stat'ing the file. */
    pr_response_add_err(R_550, "%s: %s", cmd->arg, strerror(errno));
    return ERROR(cmd);
  }

  if (session.restart_pos) {

    /* Make sure that the requested offset is valid (within the size of the
     * file being resumed.
     */
    if (session.restart_pos > sbuf.st_size) {
      pr_response_add_err(R_554, "%s: invalid REST argument", cmd->arg);
      pr_fsio_close(stor_fh);
      stor_fh = NULL;
      return ERROR(cmd);
    }

    if (pr_fsio_lseek(retr_fh, session.restart_pos,
        SEEK_SET) == (off_t) -1) {
      int _errno = errno;
      pr_fsio_close(retr_fh);
      errno = _errno;
      retr_fh = NULL;
    }

    respos = session.restart_pos;
    session.restart_pos = 0L;
  }

  /* Send the data */
  pr_data_init(cmd->arg, PR_NETIO_IO_WR);

  session.xfer.path = dir;
  session.xfer.file_size = sbuf.st_size;

  cnt_steps = session.xfer.file_size / 100;
  if (cnt_steps == 0)
    cnt_steps = 1;

  if (pr_data_open(cmd->arg, NULL, PR_NETIO_IO_WR, sbuf.st_size - respos) < 0) {
    pr_data_abort(0, TRUE);
    return ERROR(cmd);
  }

  /* Retrieve the number of bytes to retrieve, maximum, if present */
  if ((nbytes_max_retrieve = find_max_nbytes("MaxRetrieveFileSize")) == 0UL)
    have_limit = FALSE;
  else
    have_limit = TRUE;

  /* Check the MaxRetrieveFileSize.  If it is zero, or if the size
   * of the file being retrieved is greater than the MaxRetrieveFileSize,
   * then signal an error and abort the transfer now.
   */
  if (have_limit &&
      ((nbytes_max_retrieve == 0) || (sbuf.st_size > nbytes_max_retrieve))) {

    log_pri(PR_LOG_INFO, "MaxRetrieveFileSize (%" PR_LU " byte%s) reached: "
      "aborting transfer of '%s'", nbytes_max_retrieve,
      nbytes_max_retrieve != 1 ? "s" : "", dir);

    /* Abort the transfer. */
    retr_abort();

    /* Set errno to EPERM ("Operation not permitted") */
    pr_data_abort(EPERM, FALSE);
    return ERROR(cmd);
  }

  bufsize = (main_server->tcp_swin > 0 ?
             main_server->tcp_swin : PR_TUNABLE_BUFFER_SIZE);
  lbuf = (char *) palloc(cmd->tmp_pool, bufsize);

  cnt = respos;

  pr_scoreboard_update_entry(getpid(),
    PR_SCORE_XFER_SIZE, session.xfer.file_size,
    PR_SCORE_XFER_DONE, 0,
    NULL);

  while (cnt != session.xfer.file_size) {
    if (XFER_ABORTED)
      break;

    if ((len = _transmit_data(cnt, respos, lbuf, bufsize)) == 0)
      break;

    if (len < 0) {
      retr_abort();
      pr_data_abort(PR_NETIO_ERRNO(session.d->outstrm), FALSE);
      return ERROR(cmd);
    }

    cnt += len;

    if ((cnt / cnt_steps) != cnt_next) {
      cnt_next = cnt / cnt_steps;

      pr_scoreboard_update_entry(getpid(),
        PR_SCORE_XFER_DONE, cnt,
        NULL);
    }

    /* If no throttling is configured, this simply updates the scoreboard. */
    xfer_rate_throttle(cnt);
  }

  if (XFER_ABORTED) {
    retr_abort();
    pr_data_abort(0, 0);
    return ERROR(cmd);

  } else if (len < 0) {
    retr_abort();
    pr_data_abort(errno, FALSE);
    return ERROR(cmd);

  } else {

    /* If no throttling is configured, this simply updates the scoreboard. */
    xfer_rate_throttle(cnt);

    retr_complete();
    pr_data_close(FALSE);
  }

  return HANDLED(cmd);
}

MODRET xfer_abor(cmd_rec *cmd) {
  if (cmd->argc != 1) {
    pr_response_add_err(R_500, "'%s' not understood", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  pr_response_add(R_226, "Abort successful");
  pr_data_abort(0, FALSE);
  pr_data_reset();
  pr_data_cleanup();

  return HANDLED(cmd);
}

MODRET xfer_type(cmd_rec *cmd) {
  if (cmd->argc < 2 || cmd->argc > 3) {
    pr_response_add_err(R_500, "'%s' not understood", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  cmd->argv[1][0] = toupper(cmd->argv[1][0]);

  if (!strcmp(cmd->argv[1], "A") ||
      (cmd->argc == 3 && !strcmp(cmd->argv[1], "L") &&
       !strcmp(cmd->argv[2], "7"))) {

    /* TYPE A(SCII) or TYPE L 7. */
    session.sf_flags |= SF_ASCII;

  } else if (!strcmp(cmd->argv[1], "I") ||
      (cmd->argc == 3 && !strcmp(cmd->argv[1], "L") &&
       !strcmp(cmd->argv[2], "8"))) {

    /* TYPE I(MAGE) or TYPE L 8. */
    session.sf_flags &= (SF_ALL^SF_ASCII);

  } else {
    pr_response_add_err(R_500, "'%s' not understood", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  pr_response_add(R_200, "Type set to %s", cmd->argv[1]);
  return HANDLED(cmd);
}

MODRET xfer_stru(cmd_rec *cmd) {
  if (cmd->argc != 2) {
    pr_response_add_err(R_501, "'%s' not understood", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  cmd->argv[1][0] = toupper(cmd->argv[1][0]);

  switch ((int) cmd->argv[1][0]) {
    case 'F':
      /* Should 202 be returned instead??? */
      pr_response_add(R_200, "Structure set to F.");
      return HANDLED(cmd);
      break;

    case 'R':
      /* Accept R but with no operational difference from F???
       * R is required in minimum implementations by RFC-959, 5.1.
       * RFC-1123, 4.1.2.13, amends this to only apply to servers whose file
       * systems support record structures, but also suggests that such a
       * server "may still accept files with STRU R, recording the byte stream
       * literally." Another configurable choice, perhaps?
       *
       * NOTE: wu-ftpd does not so accept STRU R.
       */

       /* FALLTHROUGH */

    case 'P':
      /* RFC-1123 recommends against implementing P. */
      pr_response_add_err(R_504, "'%s' unsupported structure type.",
        get_full_cmd(cmd));
      return ERROR(cmd);
      break;

    default:
      pr_response_add_err(R_501, "'%s' unrecognized structure type.",
        get_full_cmd(cmd));
      return ERROR(cmd);
      break;
  }
}

MODRET xfer_mode(cmd_rec *cmd) {
  if (cmd->argc != 2) {
    pr_response_add_err(R_501, "'%s' not understood", get_full_cmd(cmd));
    return ERROR(cmd);
  }

  cmd->argv[1][0] = toupper(cmd->argv[1][0]);

  switch ((int) cmd->argv[1][0]) {
    case 'S':
      /* Should 202 be returned instead??? */
      pr_response_add(R_200, "Mode set to S.");
      return HANDLED(cmd);
      break;

    case 'B':
      /* FALLTHROUGH */

    case 'C':
      pr_response_add_err(R_504, "'%s' unsupported transfer mode.",
        get_full_cmd(cmd));
      return ERROR(cmd);
      break;

    default:
      pr_response_add_err(R_501, "'%s' unrecognized transfer mode.",
        get_full_cmd(cmd));
      return ERROR(cmd);
      break;
  }
}

MODRET xfer_allo(cmd_rec *cmd) {
  pr_response_add(R_202, "No storage allocation necessary.");
  return HANDLED(cmd);
}

MODRET xfer_smnt(cmd_rec *cmd) {
  pr_response_add(R_502, "SMNT command not implemented.");
  return HANDLED(cmd);
}

MODRET xfer_err_cleanup(cmd_rec *cmd) {
  if (session.xfer.p)
    destroy_pool(session.xfer.p);

  memset(&session.xfer, '\0', sizeof(session.xfer));
  return DECLINED(cmd);
}

MODRET xfer_log_stor(cmd_rec *cmd) {
  _log_transfer('i', 'c');
  pr_data_cleanup();

  /* Increment the file counters. */
  session.total_files_in++;
  session.total_files_xfer++;

  return DECLINED(cmd);
}

MODRET xfer_log_retr(cmd_rec *cmd) {
  _log_transfer('o', 'c');
  pr_data_cleanup();

  /* Increment the file counters. */
  session.total_files_out++;
  session.total_files_xfer++;

  return DECLINED(cmd);
}

static int noxfer_timeout_cb(CALLBACK_FRAME) {
  if (session.sf_flags & SF_XFER)
    /* Transfer in progress, ignore this timeout */
    return 1;

  pr_response_send_async(R_421, "No Transfer Timeout (%d seconds): closing "
    "control connection.", TimeoutNoXfer);

  remove_timer(TIMER_IDLE, ANY_MODULE);
  remove_timer(TIMER_LOGIN, ANY_MODULE);

  session_exit(PR_LOG_NOTICE, "FTP no transfer timeout, disconnected", 0, NULL);
  return 0;
}

static int xfer_sess_init(void) {
  config_rec *c = NULL;

  /* Check for a server-specific TimeoutNoTransfer */
  if ((c = find_config(main_server->conf, CONF_PARAM, "TimeoutNoTransfer",
      FALSE)) != NULL)
    TimeoutNoXfer = *((int *) c->argv[0]);

  /* Setup TimeoutNoXfer timer */
  if (TimeoutNoXfer)
    add_timer(TimeoutNoXfer, TIMER_NOXFER, &xfer_module, noxfer_timeout_cb);

  /* Check for a server-specific TimeoutStalled */
  if ((c = find_config(main_server->conf, CONF_PARAM, "TimeoutStalled",
      FALSE)) != NULL)
    TimeoutStalled = *((int *) c->argv[0]);

  /* Note: timers for handling TimeoutStalled timeouts are handled in the
   * data transfer routines, not here.
   */

  /* Exit handler for HiddenStores cleanup */
  pr_exit_register_handler(xfer_exit_cb);

  return 0;
}

/* Configuration handlers
 */

MODRET set_allowoverwrite(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|
    CONF_DIR|CONF_DYNDIR);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = (unsigned char) bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_allowrestart(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|
    CONF_DIR|CONF_DYNDIR);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_deleteabortedstores(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|
    CONF_DIR|CONF_DYNDIR);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_hiddenstores(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|CONF_DIR);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  c = add_config_param("HiddenStores", 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_maxfilesize(cmd_rec *cmd) {
  config_rec *c = NULL;
  unsigned long nbytes;
  unsigned int precedence = 0;

  int ctxt = (cmd->config && cmd->config->config_type != CONF_PARAM ?
     cmd->config->config_type : cmd->server->config_type ?
     cmd->server->config_type : CONF_ROOT);

  if (cmd->argc-1 != 2 && cmd->argc-1 != 4)
    CONF_ERROR(cmd, "incorrect number of parameters");

  CHECK_CONF(cmd, CONF_ROOT|CONF_ANON|CONF_VIRTUAL|CONF_GLOBAL|CONF_DIR|
    CONF_DYNDIR);

  /* Set the precedence for this config_rec based on its configuration
   * context.
   */
  if (ctxt & CONF_GLOBAL)
    precedence = 1;

  /* These will never appear simultaneously */
  else if (ctxt & CONF_ROOT || ctxt & CONF_VIRTUAL)
    precedence = 2;

  else if (ctxt & CONF_ANON)
    precedence = 3;

  else if (ctxt & CONF_DIR)
    precedence = 4;

  /* If the directive was used with four arguments, it means the optional
   * classifiers and expression were used.  Make sure the classifier is a valid
   * one.
   */
  if (cmd->argc-1 == 4) {
    if (!strcmp(cmd->argv[3], "user") ||
        !strcmp(cmd->argv[3], "group") ||
        !strcmp(cmd->argv[3], "class")) {

       /* no-op */

     } else
       CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown classifier used: '",
         cmd->argv[3], "'", NULL));
  }

  if (!strcmp(cmd->argv[1], "*")) {

    /* Do nothing here -- the "*" signifies an unlimited size, which is
     * what the server provides by default.
     */
    nbytes = 0UL;

  } else {

    /* Pass the cmd_rec off to see what number of bytes was
     * requested/configured.
     */
    if ((nbytes = parse_max_nbytes(cmd->argv[1], cmd->argv[2])) == 0) {
      char ulong_max[80] = {'\0'};
      sprintf(ulong_max, "%lu", ULONG_MAX);

      if (xfer_errno == EINVAL)
        CONF_ERROR(cmd, "invalid parameters");

      if (xfer_errno == ERANGE)
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
         "number of bytes must be between 0 and ", ulong_max, NULL));
    }
  }

  if (cmd->argc-1 == 2) {
    c = add_config_param(cmd->argv[0], 2, NULL, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
    *((unsigned long *) c->argv[0]) = nbytes;
    c->argv[1] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[1]) = precedence;

  } else {
    array_header *acl = NULL;
    int argc = cmd->argc - 4;
    char **argv = cmd->argv + 3;

    acl = pr_parse_expression(cmd->tmp_pool, &argc, argv);

    c = add_config_param(cmd->argv[0], 0);
    c->argc = argc + 3;
    c->argv = pcalloc(c->pool, ((argc + 4) * sizeof(char *)));

    argv = (char **) c->argv;

    /* Copy in the configured bytes */
    *argv = pcalloc(c->pool, sizeof(unsigned long));
    *((unsigned long *) *argv++) = nbytes;

    /* Copy in the precedence */
    *argv = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) *argv++) = precedence;

    /* Copy in the classifier. */
    *argv++ = pstrdup(c->pool, cmd->argv[3]);

    if (argc && acl) {
      while (argc--) {
        *argv++ = pstrdup(c->pool, *((char **) acl->elts));
        acl->elts = ((char **) acl->elts) + 1;
      }
    }

    /* Don't forget the terminating NULL */
    *argv = NULL;
  }

  c->flags |= CF_MERGEDOWN_MULTI;

  return HANDLED(cmd);
}

MODRET set_storeuniqueprefix(cmd_rec *cmd) {
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|
    CONF_DIR|CONF_DYNDIR);

  /* make sure there are no slashes in the prefix */
  if (strchr(cmd->argv[1], '/') != NULL)
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "no slashes allowed in prefix: '",
      cmd->argv[1], "'", NULL));

  c = add_config_param_str(cmd->argv[0], 1, (void *) cmd->argv[1]);
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_timeoutnoxfer(cmd_rec *cmd) {
  int timeout = -1;
  char *endp = NULL;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  timeout = (int) strtol(cmd->argv[1], &endp, 10);

  if ((endp && *endp) || timeout < 0 || timeout > 65535)
    CONF_ERROR(cmd, "timeout values must be between 0 and 65535");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = timeout;

  return HANDLED(cmd);
}

MODRET set_timeoutstalled(cmd_rec *cmd) {
  int timeout = -1;
  char *endp = NULL;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  timeout = (int) strtol(cmd->argv[1], &endp, 10);

  if ((endp && *endp) || timeout < 0 || timeout > 65535)
    CONF_ERROR(cmd, "timeout values must be between 0 and 65535");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = timeout;

  return HANDLED(cmd);
}

/* usage: TransferRate cmds kbps[:free-bytes] ["user"|"group"|"class"
 *          expression]
 */
MODRET set_transferrate(cmd_rec *cmd) {
  config_rec *c = NULL;
  char *tmp = NULL, *endp = NULL;
  long double rate = 0.0;
  off_t freebytes = 0;
  unsigned int precedence = 0;

  int ctxt = (cmd->config && cmd->config->config_type != CONF_PARAM ?
     cmd->config->config_type : cmd->server->config_type ?
     cmd->server->config_type : CONF_ROOT);

  /* Must have two or four parameters */
  if (cmd->argc-1 != 2 && cmd->argc-1 != 4)
    CONF_ERROR(cmd, "wrong number of parameters");

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON|
    CONF_DIR|CONF_DYNDIR);

  /* Set the precedence for this config_rec based on its configuration
   * context.
   */
  if (ctxt & CONF_GLOBAL)
    precedence = 1;

  /* These will never appear simultaneously */
  else if (ctxt & CONF_ROOT || ctxt & CONF_VIRTUAL)
    precedence = 2;

  else if (ctxt & CONF_ANON)
    precedence = 3;

  else if (ctxt & CONF_DIR)
    precedence = 4;

  /* Note: by tweaking this value to be lower than the precedence for
   * <Directory> appearances of this directive, I can effectively cause
   * any .ftpaccess appearances not to override...
   */
  else if (ctxt & CONF_DYNDIR)
    precedence = 5;

  /* Check for a valid classifier. */
  if (cmd->argc-1 > 2) {
    if (!strcmp(cmd->argv[3], "user") ||
        !strcmp(cmd->argv[3], "group") ||
        !strcmp(cmd->argv[3], "class"))
      /* do nothing */
      ;
    else
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown classifier requested: '",
        cmd->argv[3], "'", NULL));
  }

  if ((tmp = strchr(cmd->argv[2], ':')) != NULL)
    *tmp = '\0';

  /* Parse the 'kbps' part.  Ideally, we'd be using strtold(3) rather than
   * strtod(3) here, but FreeBSD doesn't have strtold(3).  Yay.  Portability.
   */
  rate = (long double) strtod(cmd->argv[2], &endp);

  if (rate < 0.0)
    CONF_ERROR(cmd, "rate must be greater than zero");

  if (endp && *endp)
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "invalid number: '",
      cmd->argv[2], "'", NULL));

  /* Parse any 'free-bytes' part */
  if (tmp) {
    cmd->argv[2] = ++tmp;

    if ((freebytes = strtoul(cmd->argv[2], &endp, 10)) < 0)
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
        "negative values not allowed: '", cmd->argv[2], "'", NULL));

    if (endp && *endp)
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "invalid number: '",
        cmd->argv[2], "'", NULL));
  }

  /* Construct the config_rec */
  if (cmd->argc-1 == 2) {
    c = add_config_param(cmd->argv[0], 4, NULL, NULL, NULL, NULL);

    /* Parse the command list. */
    if (!xfer_rate_parse_cmdlist(c, cmd->argv[1]))
      CONF_ERROR(cmd, "error with command list");

    c->argv[1] = pcalloc(c->pool, sizeof(long double));
    *((long double *) c->argv[1]) = rate;
    c->argv[2] = pcalloc(c->pool, sizeof(off_t));
    *((off_t *) c->argv[2]) = freebytes;
    c->argv[3] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[3]) = precedence;

  } else {
    array_header *acl = NULL;
    int argc = cmd->argc - 4;
    char **argv = cmd->argv + 3;

    acl = pr_parse_expression(cmd->tmp_pool, &argc, argv);

    c = add_config_param(cmd->argv[0], 0);

    /* Parse the command list. */

    /* The five additional slots are for: cmd-list, bps, free-bytes, precedence,
     * user/group/class.
     */
    c->argc = argc + 5;

    c->argv = pcalloc(c->pool, ((c->argc + 1) * sizeof(char *)));
    argv = (char **) c->argv;

    if (!xfer_rate_parse_cmdlist(c, cmd->argv[1]))
      CONF_ERROR(cmd, "error with command list");

    /* Note: the command list is at index 0, hence this increment. */
    argv++;

    *argv = pcalloc(c->pool, sizeof(long double));
    *((long double *) *argv++) = rate;
    *argv = pcalloc(c->pool, sizeof(off_t));
    *((unsigned long *) *argv++) = freebytes;
    *argv = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) *argv++) = precedence;

    *argv++ = pstrdup(c->pool, cmd->argv[3]);

    if (argc && acl) {
      while (argc--) {
        *argv++ = pstrdup(c->pool, *((char **) acl->elts));
        acl->elts = ((char **) acl->elts) + 1;
      }
    }

    /* don't forget the terminating NULL */
    *argv = NULL;
  }

  c->flags |= CF_MERGEDOWN_MULTI;
  return HANDLED(cmd);
}

MODRET set_ratedeprecated(cmd_rec *cmd) {
  CONF_ERROR(cmd, "deprecated.  Use TransferRate instead");
}

/* Module API tables
 */

static conftable xfer_conftab[] = {
  { "AllowOverwrite",		set_allowoverwrite,		NULL },
  { "AllowRetrieveRestart",	set_allowrestart,		NULL },
  { "AllowStoreRestart",	set_allowrestart,		NULL },
  { "DeleteAbortedStores",	set_deleteabortedstores,	NULL },
  { "HiddenStor",		set_hiddenstores,		NULL },
  { "HiddenStores",		set_hiddenstores,		NULL },
  { "MaxRetrieveFileSize",	set_maxfilesize,		NULL },
  { "MaxStoreFileSize",		set_maxfilesize,		NULL },
  { "StoreUniquePrefix",	set_storeuniqueprefix,		NULL },
  { "TimeoutNoTransfer",	set_timeoutnoxfer,		NULL },
  { "TimeoutStalled",		set_timeoutstalled,		NULL },
  { "TransferRate",		set_transferrate,		NULL },

  /* Deprecated */
  { "RateReadBPS",		set_ratedeprecated,		NULL },
  { "RateReadFreeBytes",	set_ratedeprecated,		NULL },
  { "RateReadHardBPS",          set_ratedeprecated,		NULL },
  { "RateWriteBPS",             set_ratedeprecated,		NULL },
  { "RateWriteFreeBytes",       set_ratedeprecated,		NULL },
  { "RateWriteHardBPS",         set_ratedeprecated,		NULL },

  { NULL }
};

static cmdtable xfer_cmdtab[] = {
  { CMD,     C_TYPE,	G_NONE,	 xfer_type,	TRUE,	FALSE, CL_MISC },
  { CMD,     C_STRU,	G_NONE,	 xfer_stru,	TRUE,	FALSE, CL_MISC },
  { CMD,     C_MODE,	G_NONE,	 xfer_mode,	TRUE,	FALSE, CL_MISC },
  { CMD,     C_ALLO,	G_NONE,	 xfer_allo,	TRUE,	FALSE, CL_MISC },
  { CMD,     C_SMNT,	G_NONE,	 xfer_smnt,	TRUE,	FALSE, CL_MISC },
  { PRE_CMD, C_RETR,	G_READ,	 xfer_pre_retr,	TRUE,	FALSE },
  { CMD,     C_RETR,	G_READ,	 xfer_retr,	TRUE,	FALSE, CL_READ },
  { LOG_CMD, C_RETR,	G_NONE,	 xfer_log_retr,	FALSE,  FALSE },
  { LOG_CMD_ERR, C_RETR,G_NONE,  xfer_err_cleanup,  FALSE,  FALSE },
  { PRE_CMD, C_STOR,	G_WRITE, xfer_pre_stor,	TRUE,	FALSE },
  { CMD,     C_STOR,	G_WRITE, xfer_stor,	TRUE,	FALSE, CL_WRITE },
  { LOG_CMD, C_STOR,    G_NONE,	 xfer_log_stor,	FALSE,  FALSE },
  { LOG_CMD_ERR, C_STOR,G_NONE,  xfer_err_cleanup,  FALSE,  FALSE },
  { PRE_CMD, C_STOU,	G_WRITE, xfer_pre_stou,	TRUE,	FALSE },
  { CMD,     C_STOU,	G_WRITE, xfer_stor,	TRUE,	FALSE, CL_WRITE },
  { POST_CMD,C_STOU,	G_WRITE, xfer_post_stou,FALSE,	FALSE },
  { LOG_CMD, C_STOU,	G_NONE,  xfer_log_stor,	FALSE,	FALSE },
  { LOG_CMD_ERR, C_STOU,G_NONE,  xfer_err_cleanup,  FALSE,  FALSE },
  { PRE_CMD, C_APPE,	G_WRITE, xfer_pre_appe,	TRUE,	FALSE },
  { CMD,     C_APPE,	G_WRITE, xfer_stor,	TRUE,	FALSE, CL_WRITE },
  { LOG_CMD, C_APPE,	G_NONE,  xfer_log_stor,	FALSE,  FALSE },
  { LOG_CMD_ERR, C_APPE,G_NONE,  xfer_err_cleanup,  FALSE,  FALSE },
  { CMD,     C_ABOR,	G_NONE,	 xfer_abor,	TRUE,	TRUE,  CL_MISC  },
  { CMD,     C_REST,	G_NONE,	 xfer_rest,	TRUE,	FALSE, CL_MISC  },
  { PRE_CMD, C_CDUP,	G_NONE,  xfer_reset_rate, TRUE,	FALSE },
  { PRE_CMD, C_CWD,	G_NONE,  xfer_reset_rate, TRUE,	FALSE },
  { PRE_CMD, C_XCUP,	G_NONE,  xfer_reset_rate, TRUE,	FALSE },
  { PRE_CMD, C_XCWD,	G_NONE,  xfer_reset_rate, TRUE,	FALSE },
  { 0,NULL }
};

module xfer_module = {
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "xfer",

  /* Module configuration directive table */
  xfer_conftab,

  /* Module command handler table */
  xfer_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization function */
  NULL,

  /* Session initialization function */
  xfer_sess_init
};
