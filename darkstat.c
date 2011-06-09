/* darkstat 3
 * copyright (c) 2001-2011 Emil Mikulic.
 *
 * darkstat.c: signals, cmdline parsing, program body.
 *
 * You may use, modify and redistribute this file under the terms of the
 * GNU General Public License version 2. (see COPYING.GPL)
 */

#include "acct.h"
#include "cap.h"
#include "cdefs.h"
#include "config.h"
#include "conv.h"
#include "daylog.h"
#include "db.h"
#include "dns.h"
#include "err.h"
#include "http.h"
#include "hosts_db.h"
#include "localip.h"
#include "ncache.h"
#include "pidfile.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <pcap.h>

#include "now.h"
time_t now;

#ifndef INADDR_NONE
# define INADDR_NONE (-1) /* Solaris */
#endif

/* --- Signal handling --- */
static volatile int running = 1;
static void sig_shutdown(int signum _unused_) { running = 0; }

static volatile int reset_pending = 0, export_pending = 0;
static void sig_reset(int signum _unused_)
{
   reset_pending = 1;
   export_pending = 1;
}

static void sig_export(int signum _unused_) { export_pending = 1; }

/* --- Commandline parsing --- */
static unsigned long
parsenum(const char *str, unsigned long max /* 0 for no max */)
{
   unsigned long n;
   char *end;

   errno = 0;
   n = strtoul(str, &end, 10);
   if (*end != '\0')
      errx(1, "\"%s\" is not a valid number", str);
   if (errno == ERANGE)
      errx(1, "\"%s\" is out of range", str);
   if ((max != 0) && (n > max))
      errx(1, "\"%s\" is out of range (max %lu)", str, max);
   return n;
}

const char *opt_interface = NULL;
static void cb_interface(const char *arg) { opt_interface = arg; }

const char *opt_capfile = NULL;
static void cb_capfile(const char *arg) { opt_capfile = arg; }

int opt_want_snaplen = -1;
static void cb_snaplen(const char *arg)
{ opt_want_snaplen = (int)parsenum(arg, 0); }

int opt_want_pppoe = 0;
static void cb_pppoe(const char *arg _unused_) { opt_want_pppoe = 1; }

int opt_want_syslog = 0;
static void cb_syslog(const char *arg _unused_) { opt_want_syslog = 1; }

int opt_want_verbose = 0;
static void cb_verbose(const char *arg _unused_) { opt_want_verbose = 1; }

int opt_want_daemonize = 1;
static void cb_no_daemon(const char *arg _unused_) { opt_want_daemonize = 0; }

int opt_want_promisc = 1;
static void cb_no_promisc(const char *arg _unused_) { opt_want_promisc = 0; }

int opt_want_dns = 1;
static void cb_no_dns(const char *arg _unused_) { opt_want_dns = 0; }

int opt_want_macs = 1;
static void cb_no_macs(const char *arg _unused_) { opt_want_macs = 0; }

int opt_want_lastseen = 1;
static void cb_no_lastseen(const char *arg _unused_) { opt_want_lastseen = 0; }

unsigned short opt_bindport = 667;
static void cb_port(const char *arg)
{ opt_bindport = (unsigned short)parsenum(arg, 65536); }

static void cb_bindaddr(const char *arg) { http_add_bindaddr(arg); }

const char *opt_filter = NULL;
static void cb_filter(const char *arg) { opt_filter = arg; }

static void cb_local(const char *arg) { acct_init_localnet(arg); }

const char *opt_chroot_dir = NULL;
static void cb_chroot(const char *arg) { opt_chroot_dir = arg; }

const char *opt_privdrop_user = NULL;
static void cb_user(const char *arg) { opt_privdrop_user = arg; }

const char *daylog_fn = NULL;
static void cb_daylog(const char *arg)
{
   if (opt_chroot_dir == NULL)
      errx(1, "the daylog file is relative to the chroot.\n"
      "You must specify a --chroot dir before you can use --daylog.");
   else
      daylog_fn = arg;
}

const char *import_fn = NULL;
static void cb_import(const char *arg)
{
   if (opt_chroot_dir == NULL)
      errx(1, "the import file is relative to the chroot.\n"
      "You must specify a --chroot dir before you can use --import.");
   else
      import_fn = arg;
}

const char *export_fn = NULL;
static void cb_export(const char *arg)
{
   if ((opt_chroot_dir == NULL) && (opt_capfile == NULL))
      errx(1, "the export file is relative to the chroot.\n"
      "You must specify a --chroot dir before you can use --export.");
   else
      export_fn = arg;
}

static const char *pid_fn = NULL;
static void cb_pidfile(const char *arg)
{
   if (opt_chroot_dir == NULL)
      errx(1, "the pidfile is relative to the chroot.\n"
      "You must specify a --chroot dir before you can use --pidfile.");
   else
      pid_fn = arg;
}

unsigned int opt_hosts_max = 1000;
static void cb_hosts_max(const char *arg)
{ opt_hosts_max = parsenum(arg, 0); }

unsigned int opt_hosts_keep = 500;
static void cb_hosts_keep(const char *arg)
{ opt_hosts_keep = parsenum(arg, 0); }

unsigned int opt_ports_max = 200;
static void cb_ports_max(const char *arg)
{ opt_ports_max = parsenum(arg, 65536); }

unsigned int opt_ports_keep = 30;
static void cb_ports_keep(const char *arg)
{ opt_ports_keep = parsenum(arg, 65536); }

unsigned int opt_highest_port = 65535;
static void cb_highest_port(const char *arg)
{ opt_highest_port = parsenum(arg, 65535); }

int opt_wait_secs = -1;
static void cb_wait_secs(const char *arg)
{ opt_wait_secs = (int)parsenum(arg, 0); }

int opt_want_hexdump = 0;
static void cb_hexdump(const char *arg _unused_)
{ opt_want_hexdump = 1; }

int opt_want_help = 0;
static void cb_help(const char *arg _unused_)
{ opt_want_help = 1; }
static void cb_version(const char *arg _unused_)
{ opt_want_help = -1; }

/* --- */

struct cmdline_arg {
   const char *name, *arg_name; /* NULL arg_name means unary */
   void (*callback)(const char *arg);
   int num_seen;
};

static struct cmdline_arg cmdline_args[] = {
   {"-i",             "interface",       cb_interface,    0},
   {"-r",             "file",            cb_capfile,      0},
   {"-p",             "port",            cb_port,         0},
   {"-b",             "bindaddr",        cb_bindaddr,    -1},
   {"-f",             "filter",          cb_filter,       0},
   {"-l",             "network/netmask", cb_local,        0},
   {"--snaplen",      "bytes",           cb_snaplen,      0},
   {"--pppoe",        NULL,              cb_pppoe,        0},
   {"--syslog",       NULL,              cb_syslog,       0},
   {"--verbose",      NULL,              cb_verbose,      0},
   {"--no-daemon",    NULL,              cb_no_daemon,    0},
   {"--no-promisc",   NULL,              cb_no_promisc,   0},
   {"--no-dns",       NULL,              cb_no_dns,       0},
   {"--no-macs",      NULL,              cb_no_macs,      0},
   {"--no-lastseen",  NULL,              cb_no_lastseen,  0},
   {"--chroot",       "dir",             cb_chroot,       0},
   {"--user",         "username",        cb_user,         0},
   {"--daylog",       "filename",        cb_daylog,       0},
   {"--import",       "filename",        cb_import,       0},
   {"--export",       "filename",        cb_export,       0},
   {"--pidfile",      "filename",        cb_pidfile,      0},
   {"--hosts-max",    "count",           cb_hosts_max,    0},
   {"--hosts-keep",   "count",           cb_hosts_keep,   0},
   {"--ports-max",    "count",           cb_ports_max,    0},
   {"--ports-keep",   "count",           cb_ports_keep,   0},
   {"--highest-port", "port",            cb_highest_port, 0},
   {"--wait",         "secs",            cb_wait_secs,    0},
   {"--hexdump",      NULL,              cb_hexdump,      0},
   {"--version",      NULL,              cb_version,      0},
   {"--help",         NULL,              cb_help,         0},
   {NULL,             NULL,              NULL,            0}
};

/*
 * We autogenerate the usage statement from the cmdline_args data structure.
 */
static void
usage(void)
{
   static char intro[] = "usage: darkstat ";
   char indent[sizeof(intro)];
   struct cmdline_arg *arg;

   printf(PACKAGE_STRING " (using %s)\n", pcap_lib_version());
   if (opt_want_help == -1) return;

   memset(indent, ' ', sizeof(indent));
   indent[0] = indent[sizeof(indent) - 1] = 0;

   printf("\n%s", intro);
   for (arg = cmdline_args; arg->name != NULL; arg++) {
      printf("%s[ %s%s%s ]\n",
          indent,
          arg->name,
          arg->arg_name != NULL ? " " : "",
          arg->arg_name != NULL ? arg->arg_name : "");
      indent[0] = ' ';
   }
   printf("\n"
"Please refer to the darkstat(8) manual page for further\n"
"documentation and usage examples.\n");
}

static void
parse_sub_cmdline(const int argc, char * const *argv)
{
   struct cmdline_arg *arg;

   if (argc == 0) return;
   for (arg = cmdline_args; arg->name != NULL; arg++)
      if (strcmp(argv[0], arg->name) == 0) {
         if ((arg->arg_name != NULL) && (argc == 1)) {
            fprintf(stderr,
               "error: argument \"%s\" requires parameter \"%s\"\n",
               arg->name, arg->arg_name);
            usage();
            exit(EXIT_FAILURE);
         }
         if (arg->num_seen > 0) {
            fprintf(stderr,
               "error: already specified argument \"%s\"\n",
               arg->name);
            usage();
            exit(EXIT_FAILURE);
         }

         if (arg->num_seen != -1) /* accept more than one */
            arg->num_seen++;

         if (arg->arg_name == NULL) {
            arg->callback(NULL);
            parse_sub_cmdline(argc-1, argv+1);
         } else {
            arg->callback(argv[1]);
            parse_sub_cmdline(argc-2, argv+2);
         }
         return;
      }

   fprintf(stderr, "error: illegal argument: \"%s\"\n", argv[0]);
   usage();
   exit(EXIT_FAILURE);
}

static void
parse_cmdline(const int argc, char * const *argv)
{
   if (argc < 1) {
      /* Not enough args. */
      usage();
      exit(EXIT_FAILURE);
   }

   parse_sub_cmdline(argc, argv);

   if (opt_want_help) {
     usage();
     exit(EXIT_SUCCESS);
   }

   /* start syslogging as early as possible */
   if (opt_want_syslog) openlog("darkstat", LOG_NDELAY | LOG_PID, LOG_DAEMON);

   /* some default values */
   if (opt_chroot_dir == NULL) opt_chroot_dir = CHROOT_DIR;
   if (opt_privdrop_user == NULL) opt_privdrop_user = PRIVDROP_USER;

   /* sanity check args */
   if ((opt_interface == NULL) && (opt_capfile == NULL))
      errx(1, "must specify either interface (-i) or capture file (-r)");

   if ((opt_interface != NULL) && (opt_capfile != NULL))
      errx(1, "can't specify both interface (-i) and capture file (-r)");

   if ((opt_hosts_max != 0) && (opt_hosts_keep >= opt_hosts_max)) {
      opt_hosts_keep = opt_hosts_max / 2;
      warnx("reducing --hosts-keep to %u, to be under --hosts-max (%u)",
         opt_hosts_keep, opt_hosts_max);
   }
   verbosef("max %u hosts, cutting down to %u when exceeded",
      opt_hosts_max, opt_hosts_keep);

   if ((opt_ports_max != 0) && (opt_ports_keep >= opt_ports_max)) {
      opt_ports_keep = opt_ports_max / 2;
      warnx("reducing --ports-keep to %u, to be under --ports-max (%u)",
         opt_ports_keep, opt_ports_max);
   }
   verbosef("max %u ports per host, cutting down to %u when exceeded",
      opt_ports_max, opt_ports_keep);

   if (opt_want_hexdump && !opt_want_verbose) {
      opt_want_verbose = 1;
      verbosef("--hexdump implies --verbose");
   }

   if (opt_want_hexdump && opt_want_daemonize) {
      opt_want_daemonize = 0;
      verbosef("--hexdump implies --no-daemon");
   }
}

static void
run_from_capfile(void)
{
   graph_init();
   hosts_db_init();
   cap_from_file(opt_capfile, opt_filter);
   cap_stop();
   if (export_fn != NULL) db_export(export_fn);
   hosts_db_free();
   graph_free();
#ifndef PRIu64
#warning "PRIu64 is not defined, using qu instead"
#define PRIu64 "qu"
#endif
   verbosef("Total packets: %"PRIu64", bytes: %"PRIu64,
      acct_total_packets, acct_total_bytes);
}

/* --- Program body --- */
int
main(int argc, char **argv)
{
   test_64order();
   parse_cmdline(argc-1, argv+1);

   if (opt_capfile) {
      /*
       * This is very different from a regular run against a network
       * interface.
       */
      run_from_capfile();
      return 0;
   }

   /* must verbosef() before first fork to init lock */
   verbosef("starting up");
   if (pid_fn) pidfile_create(opt_chroot_dir, pid_fn, opt_privdrop_user);

   if (opt_want_daemonize) {
      verbosef("daemonizing to run in the background!");
      daemonize_start();
      verbosef("I am the main process");
   }
   if (pid_fn) pidfile_write_close();

   /* do this first as it forks - minimize memory use */
   if (opt_want_dns) dns_init(opt_privdrop_user);
   cap_init(opt_interface, opt_filter, opt_want_promisc); /* needs root */
   http_listen(opt_bindport);
   ncache_init(); /* must do before chroot() */

   privdrop(opt_chroot_dir, opt_privdrop_user);

   /* Don't need root privs for these: */
   now = time(NULL);
   if (daylog_fn != NULL) daylog_init(daylog_fn);
   graph_init();
   hosts_db_init();
   if (import_fn != NULL) db_import(import_fn);
   localip_init(opt_interface);

   if (signal(SIGTERM, sig_shutdown) == SIG_ERR)
      errx(1, "signal(SIGTERM) failed");
   if (signal(SIGINT, sig_shutdown) == SIG_ERR)
      errx(1, "signal(SIGINT) failed");
   if (signal(SIGUSR1, sig_reset) == SIG_ERR)
      errx(1, "signal(SIGUSR1) failed");
   if (signal(SIGUSR2, sig_export) == SIG_ERR)
      errx(1, "signal(SIGUSR2) failed");

   verbosef("entering main loop");
   daemonize_finish();

   while (running) {
      int select_ret, max_fd = -1, use_timeout = 0;
      struct timeval timeout;
      fd_set rs, ws;

      now = time(NULL);

      if (export_pending) {
         if (export_fn != NULL)
            db_export(export_fn);
         export_pending = 0;
      }

      if (reset_pending) {
         hosts_db_reset();
         graph_reset();
         reset_pending = 0;
      }

      FD_ZERO(&rs);
      FD_ZERO(&ws);

      cap_fd_set(&rs, &max_fd, &timeout, &use_timeout);
      http_fd_set(&rs, &ws, &max_fd, &timeout, &use_timeout);

      select_ret = select(max_fd+1, &rs, &ws, NULL,
         (use_timeout) ? &timeout : NULL);

      if ((select_ret == 0) && (!use_timeout))
            errx(1, "select() erroneously timed out");

      if (select_ret == -1) {
         if (errno == EINTR)
            continue;
         else
            err(1, "select()");
      }
      else {
         graph_rotate();
         cap_poll(&rs);
         dns_poll();
         http_poll(&rs, &ws);
      }
   }

   verbosef("shutting down");
   verbosef("pcap stats: %u packets received, %u packets dropped",
      cap_pkts_recv, cap_pkts_drop);
   http_stop();
   cap_stop();
   dns_stop();
   if (export_fn != NULL) db_export(export_fn);
   hosts_db_free();
   graph_free();
   if (daylog_fn != NULL) daylog_free();
   ncache_free();
   if (pid_fn) pidfile_unlink();
   verbosef("shut down");
   return (EXIT_SUCCESS);
}

/* vim:set ts=3 sw=3 tw=78 expandtab: */
