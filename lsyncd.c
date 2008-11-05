/** -*- tab-width: 2; -*-
 * lsyncd.c   Live (Mirror) Syncing Demon
 *
 * License: GPLv2 (see COPYING) or any later version
 *
 * Authors: Axel Kittenberger <axel.kittenberger@univie.ac.at>
 *          Eugene Sanivsky <eugenesan@gmail.com>
 */

#include "config.h"
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef HAVE_SYS_INOTIFY_H
#  include <sys/inotify.h>
#else
#  include "inotify-nosys.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <getopt.h>
#include <assert.h>

#define INOTIFY_BUF_LEN     (512 * (sizeof(struct inotify_event) + 16))

#define LOG_DEBUG  1
#define LOG_NORMAL 2
#define LOG_ERROR  3

int loglevel = LOG_NORMAL;

/**
 * Option: if true rsync will not be actually called.
 */
int flag_dryrun = 0;

/**
 * Option: if true, do not detach and log to stdout/stderr.
 */
int flag_nodaemon = 0;

/**
 * Option: Source dir to watch
 */
char * option_source = NULL;

/**
 * Option: Target to rsync to.
 */
char * option_target = NULL;

/**
 * Option: rsync binary to call.
 */
char * rsync_binary = "/usr/bin/rsync";

/**
 * Option: the exclude-file to pass to rsync.
 */
char * exclude_file = NULL;

/**
 * Option: pidfile, which holds the PID of the running daemon process
 */
char * pidfile = NULL;

/**
 * Structure to store the directory watches of the deamon.
 */

struct dir_watch {
	/**
	 * The watch descriptor returned by kernel.
	 */
	int wd;

	/**
	 * The name of the directory.
	 * In case of the root dir to be watched, it is a full path
	 * and parent == NULL. Otherwise its just the name of the
	 * directory and parent points to the parent directory thats
	 * also watched.
	 */
	char * dirname;

	/**
	 * Call this directory that way on destiation.
	 * if NULL call it like dirname.
	 */
	char * destname;

	/**
	 * Points to the index of the parent.
	 * -1 if no parent
	 */
	int parent;
};


/**
 * Structure to store strings for the diversve Inotfy masked events.
 * Actually used for compfortable debugging only.
 */

struct inotify_mask_text {
	int mask;
	char const * text;
};

/**
 * A constant that assigns every inotify mask a printable string.
 * Used for debugging.
 */

struct inotify_mask_text mask_texts[] = {
	{ IN_ACCESS,        "ACCESS"        }, 
	{ IN_ATTRIB,        "ATTRIB"        }, 
	{ IN_CLOSE_WRITE,   "CLOSE_WRITE"   }, 
	{ IN_CLOSE_NOWRITE, "CLOSE_NOWRITE" }, 
	{ IN_CREATE,        "CREATE"        }, 
	{ IN_DELETE,        "DELETE"        }, 
	{ IN_DELETE_SELF,   "DELETE_SELF"   }, 
	{ IN_IGNORED,       "IGNORED"       }, 
	{ IN_MODIFY,        "MODIFY"        }, 
	{ IN_MOVE_SELF,     "MOVE_SELF"     }, 
	{ IN_MOVED_FROM,    "MOVED_FROM"    }, 
	{ IN_MOVED_TO,      "MOVED_TO"      }, 
	{ IN_OPEN,          "OPEN"          }, 
	{ 0, "" },
};

/**
 * Holds an allocated array of all directories watched.
 */

struct dir_watch *dir_watches;

/**
 * The allocated size of dir_watches;
 */
int dir_watch_size = 0;

/**
 * The largest dir_watch number used;
 */
int dir_watch_num = 0;

/**
 * lsyncd will log into this file/stream.
 */
char * logfile = "/var/log/lsyncd";


/**
 * The inotify instance.
 */
int inotf;

/**
 * Possible Exit codes for this application
 */
enum lsyncd_exit_code
{
	LSYNCD_SUCCESS = 0, 
	LSYNCD_OUTOFMEMORY = 1, 			/* out-of memory */
	LSYNCD_FILENOTFOUND = 2, 			/* file was not found, or failed to write */
	LSYNCD_EXECRSYNCFAIL = 3,			/* rsync execution somehow failed */
	LSYNCD_NOTENOUGHARGUMENTS = 4, /* Not enough command-line arguments were given to lsyncd invocation */
	LSYNCD_TOOMANYDIRECTORYEXCLUDES = 5, /* Too many excludes files were specified */
	LSYNCD_INTERNALFAIL = 255,    /* Internal exit code to denote that execv failed */
};

	


/**
 * Array of strings of directory names to include.
 * This is limited to MAX_EXCLUDES.
 * It's not worth to code a dynamic size handling...
 */
#define MAX_EXCLUDES 256
char * exclude_dirs[MAX_EXCLUDES] = {NULL, };
int exclude_dir_n = 0;

volatile sig_atomic_t keep_going = 1;

void catch_alarm(int sig)
{
	keep_going = 0;
}

/**
 * Prints a message to the log stream, preceding a timestamp.
 * Otherwise it behaves like printf();
 */
void printlogf(int level, const char *fmt, ...)
{
	va_list ap;
	char * ct;
	time_t mtime;
	FILE * flog;

	if (level < loglevel) {
		return;
	}

	if (!flag_nodaemon) {
		flog = fopen(logfile, "a");

		if (flog == NULL) {
			printf("cannot open logfile [%s]!\n", logfile);
			exit(LSYNCD_FILENOTFOUND);
		}
	} else {
		flog = stdout;
	}

	va_start(ap, fmt);

	time(&mtime);
	ct = ctime(&mtime);
	ct[strlen(ct) - 1] = 0; // cut trailing \n
	fprintf(flog, "%s: ", ct);

	switch (level) {

	case LOG_DEBUG  :
		break;

	case LOG_NORMAL :
		break;

	case LOG_ERROR  :
		fprintf(flog, "ERROR: ");
		break;
	}

	vfprintf(flog, fmt, ap);

	fprintf(flog, "\n");
	va_end(ap);

	if (!flag_nodaemon) {
		fclose(flog);
	}
}

/**
 * "secured" malloc, meaning the deamon shall kill itself
 * in case of out of memory.
 *
 * On linux systems, which is actually the only system this
 * deamon will run at, due to the use of inotify, this is
 * an "academic" cleaness only, linux will never return out
 * memory, but kill a process to ensure memory will be
 * available.
 */
void *s_malloc(size_t size)
{
	void *r = malloc(size);

	if (r == NULL) {
		printlogf(LOG_ERROR, "Out of memory!");
		exit(LSYNCD_OUTOFMEMORY);
	}

	return r;
}

/**
 * "secured" calloc.
 */
void *s_calloc(size_t nmemb, size_t size)
{
	void *r = calloc(nmemb, size);

	if (r == NULL) {
		printlogf(LOG_ERROR, "Out of memory!");
		exit(LSYNCD_OUTOFMEMORY);
	}

	return r;
}

/**
 * "secured" realloc.
 */
void *s_realloc(void *ptr, size_t size)
{
	void *r = realloc(ptr, size);

	if (r == NULL) {
		printlogf(LOG_ERROR, "Out of memory!");
		exit(LSYNCD_OUTOFMEMORY);
	}

	return r;
}

/**
 * "secured" strdup.
 */
char *s_strdup(const char* src)
{
	char *s = strdup(src);

	if (s == NULL) {
		printlogf(LOG_ERROR, "Out of memory!");
		exit(LSYNCD_OUTOFMEMORY);
	}

	return s;
}

/**
 * Returns the canonicalized path of a directory with a final '/', 
 * Makes sure it is a directory.
 */
char *realdir(const char * dir) 
{
	char* cs = s_malloc(PATH_MAX+1);
	cs = realpath(dir, cs);

	if (cs == NULL) {
		return NULL;
	}

	if (strlen(cs) + 1 >= PATH_MAX) {
		// at systems maxpath already, we cannot add a '/' anyway.
		return NULL;
	}

	struct stat st;
	stat(cs, &st);
	if (!S_ISDIR(st.st_mode)) {
		free(cs);
		return NULL;
	}

	strcat(cs, "/");
	return cs;
}

/**
 * Calls rsync to sync from src to dest.
 * Returns after rsync has finished.
 *
 * @param src       Source string.
 * @param dest      Destination string,
 * @param recursive If true -r will be handled on, -d (single directory) otherwise
 * @return true if successful, false if not.
 */
bool rsync(char const * src, const char * dest, bool recursive)
{
	pid_t pid;
	int status;
	char const * opts = recursive ? "-ltr" : "-ltd";
	const int MAX_ARGS = 100;
	char * argv[MAX_ARGS];
	int argc=0;
	int i;

	argv[argc++] = s_strdup(rsync_binary);
	argv[argc++] = s_strdup("--delete");
	argv[argc++] = s_strdup(opts);
	if (exclude_file) {
		argv[argc++] = s_strdup("--exclude-from");
		argv[argc++] = s_strdup(exclude_file);
	}
	argv[argc++] = s_strdup(src);
	argv[argc++] = s_strdup(dest);
	argv[argc++] = NULL;

	if (argc > MAX_ARGS) {				/* check for error condition */
		printlogf(LOG_ERROR, "Internal error: too many (%i) options passed", argc);
		return false;
	}

	/* debug dump of command-line options */
	for (i=0; i<argc; ++i) {
		printlogf(LOG_DEBUG, "exec parameter %i:%s", i, argv[i]);
	}

	if (flag_dryrun) {
		return true;
	}

	pid = fork();

	if (pid == 0) {
		if (!flag_nodaemon) {
			freopen(logfile, "a", stdout);
			freopen(logfile, "a", stderr);
		}

		execv(rsync_binary, argv);

		printlogf(LOG_ERROR, "Failed executing [%s]", rsync_binary);

		exit(LSYNCD_INTERNALFAIL);
	}

	for (i=0; i<argc; ++i) {
		if (argv[i])
			free(argv[i]);
	}
	
	waitpid(pid, &status, 0);
	assert(WIFEXITED(status));
	if (WEXITSTATUS(status)==LSYNCD_INTERNALFAIL){
		printlogf(LOG_ERROR, "Fork exit code of %i, execv failure", WEXITSTATUS(status));
		return false;
	} else if (WEXITSTATUS(status)) {
		printlogf(LOG_NORMAL, "Forked rsync process returned non-zero return code: %i", WEXITSTATUS(status));
		return false;
	}

	printlogf(LOG_DEBUG, "Rsync of [%s] -> [%s] finished", src, dest);
	return true;
}



/**
 * Adds a directory to watch
 *
 * @param pathname the absolute path of the directory to watch.
 * @param dirname  the name of the directory only (yes this is a bit redudant, but oh well)
 * @param destname if not NULL call this dir that way on destionation.
 * @param parent   if not -1 the index to the parent directory that is already watched
 *
 * @return index to dir_watches of the new dir, -1 on error.
 */
int add_watch(char const * pathname, char const * dirname, char const * destname, int parent)
{
	int wd;
	char * nn;
	int newdw;

	wd = inotify_add_watch(inotf, pathname,
			       IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF |
			       IN_MOVED_FROM | IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR);

	if (wd == -1) {
		printlogf(LOG_ERROR, "Cannot add watch %s (%d:%s)", pathname, errno, strerror(errno));
		return -1;
	}

	// look if an unused slot can be found.
	for (newdw = 0; newdw < dir_watch_num; newdw++) {
		if (dir_watches[newdw].wd < 0) {
			break;
		}
	}

	if (newdw == dir_watch_num) {
		if (dir_watch_num + 1 >= dir_watch_size) {
			dir_watch_size *= 2;
			dir_watches = s_realloc(dir_watches, dir_watch_size * sizeof(struct dir_watch));
		}

		dir_watch_num++;
	}

	dir_watches[newdw].wd = wd;

	dir_watches[newdw].parent = parent;

	nn = s_malloc(strlen(dirname) + 1);
	strcpy(nn, dirname);
	dir_watches[newdw].dirname = nn;

	if (destname) {
		nn = s_malloc(strlen(destname) + 1);
		strcpy(nn, destname);
		dir_watches[newdw].destname = nn;
	} else {
		dir_watches[newdw].destname = NULL;
	}

	return newdw;
}


/**
 * Builds the abolute path name of a given directory beeing watched from the dir_watches information.
 *
 * @param pathname      destination buffer to store the result to.
 * @param pathsize      max size of this buffer
 * @param watch         the index in dir_watches to the directory.
 * @param dirname       if not NULL it is added at the end of pathname
 * @param prefix        if not NULL it is added at the beginning of pathname
 */
bool buildpath(char *pathname,
	       int pathsize,
	       int watch,
	       char const *dirname,
	       char const *prefix)
{
	int j, k, p, ps;

	pathname[0] = 0;

	if (prefix) {
		// make sure nobody calls me with %wwwuser option set
		assert(strcmp(prefix, "%wwwuser"));
		strcat(pathname, prefix);
	}

	// count how big the parent stack is
	for (p = watch, ps = 0; p != -1; p = dir_watches[p].parent, ps++) {
	}

	// now add the parent paths from back to front
	for (j = ps; j > 0; j--) {
		char * tmpname;
		// go j steps behind stack

		for (p = watch, k = 0; k + 1 < j; p = dir_watches[p].parent, k++) {
		}

		tmpname = (prefix && dir_watches[p].destname) ? dir_watches[p].destname : dir_watches[p].dirname;

		if (strlen(pathname) + strlen(tmpname) + 2 >= pathsize) {
			printlogf(LOG_ERROR, "path too long %s/...", tmpname);
			return false;
		}

		strcat(pathname, tmpname);

		strcat(pathname, "/");
	}

	if (dirname) {
		if (strlen(pathname) + strlen(dirname) + 2 >= pathsize) {
			printlogf(LOG_ERROR, "path too long %s//%s", pathname, dirname);
			return false;
		}

		strcat(pathname, dirname);
	}

	return true;
}

/**
 * Adds a dir to watch.
 *
 * @param dirname   The name or absolute path of the directory to watch.
 * @param destname  If not NULL call this dir that way on sync destination.
 * @param recursive If true, will also watch all sub-directories.
 * @param parent    If not -1, the index in dir_watches to the parent directory already watched.
 *                  Must have absolute path if parent == -1.
 */
bool add_dirwatch(char const * dirname, char const * destname, bool recursive, int parent)
{
	DIR *d;

	struct dirent *de;
	int dw, i;
	char pathname[PATH_MAX+1];

	printlogf(LOG_DEBUG, "add_dirwatch(%s, %s, %d, p->dirname:%s)", dirname, destname, recursive, parent >= 0 ? dir_watches[parent].dirname : "NULL");

	if (!buildpath(pathname, sizeof(pathname), parent, dirname, NULL)) {
		return false;
	}

	for (i = 0; i < exclude_dir_n; i++) {
		if (!strcmp(dirname, exclude_dirs[i])) {
			return true;
		}
	}

	dw = add_watch(pathname, dirname, destname, parent);

	if (dw == -1) {
		return false;
	}

	if (strlen(pathname) + strlen(dirname) + 2 > sizeof(pathname)) {
		printlogf(LOG_ERROR, "pathname too long %s//%s", pathname, dirname);
		return false;
	}

	d = opendir(pathname);

	if (d == NULL) {
		printlogf(LOG_ERROR, "cannot open dir %s.", dirname);
		return false;
	}

	while (keep_going) {
		de = readdir(d);

		if (de == NULL) {
			break;
		}

		if (de->d_type == DT_DIR && strcmp(de->d_name, "..") && strcmp(de->d_name, ".")) {
			add_dirwatch(de->d_name, NULL, true, dw);
			//TODO call sync_dir() to sync the new directory.
		}
	}

	closedir(d);

	return true;
}

/**
 * Removes a watched dir, including recursevily all subdirs.
 *
 * @param name   Optionally. If not NULL, the directory name to remove which is a child of parent.
 * @param parent The index to the parent directory of the directory 'name' to remove,
 *               or to be removed itself if name == NULL.
 */
bool remove_dirwatch(const char * name, int parent)
{
	int i;
	int dw;

	if (name) {
		// look for the child with the name
		for (i = 0; i < dir_watch_num; i++) {
			if (dir_watches[i].wd >= 0 && dir_watches[i].parent == parent &&
					!strcmp(name, dir_watches[i].dirname)) {
				dw = i;
				break;
			}
		}

		if (i >= dir_watch_num) {
			printlogf(LOG_ERROR, "Cannot find entry for %s:/:%s :-(", dir_watches[parent].dirname, name);
			return false;
		}
	} else {
		dw = parent;
	}

	for (i = 0; i < dir_watch_num; i++) {
		if (dir_watches[i].wd >= 0 && dir_watches[i].parent == dw) {
			remove_dirwatch(NULL, i);
		}
	}

	inotify_rm_watch(inotf, dir_watches[dw].wd);

	dir_watches[dw].wd = -1;

	free(dir_watches[dw].dirname);
	dir_watches[dw].dirname = NULL;

	if (dir_watches[dw].destname) {
		free(dir_watches[dw].destname);
		dir_watches[dw].destname = NULL;
	}

	return true;
}

/**
 * Find the matching dw entry from wd (watch descriptor), and return
 * the offset in the table.
 *
 * @param wd   The wd (watch descriptor) given by inotify
 * @return offset, or -1 if not found
 */
int get_dirwatch_offset(int wd) {
	int i;
	for (i = 0; i < dir_watch_num; i++) {
		if (dir_watches[i].wd == wd) {
			break;
		}
	}

	if (i >= dir_watch_num) {
		return -1;
	} else {
		return i;
	}	
}

/**
 * Handles an inotify event.
 *
 * @param event   The event to handle
 */
bool handle_event(struct inotify_event *event)
{
	char masktext[255] = {0,};
	char pathname[PATH_MAX+1];
	char destname[PATH_MAX+1];

	int mask = event->mask;
	int i;

	struct inotify_mask_text *p;
	
	for (p = mask_texts; p->mask; p++) {
		if (mask & p->mask) {
			if (strlen(masktext) + strlen(p->text) + 3 >= sizeof(masktext)) {
				printlogf(LOG_ERROR, "bufferoverflow in handle_event");
				return false;
			}

			if (*masktext) {
				strcat(masktext, ", ");
			}

			strcat(masktext, p->text);
		}
	}
	printlogf(LOG_DEBUG, "inotfy event: %s:%s", masktext, event->name);

	if (IN_IGNORED & event->mask) {
		return true;
	}

	for (i = 0; i < exclude_dir_n; i++) {
		if (!strcmp(event->name, exclude_dirs[i])) {
			return true;
		}
	}

	i = get_dirwatch_offset(event->wd);
	if (i == -1) {
		printlogf(LOG_ERROR, "received an inotify event that doesnt match any watched directory :-(%d,%d)", event->mask, event->wd);
		return false;
	}

	if (((IN_CREATE | IN_MOVED_TO) & event->mask) && (IN_ISDIR & event->mask)) {
		add_dirwatch(event->name, NULL, false, i);
	}

	if (((IN_DELETE | IN_MOVED_FROM) & event->mask) && (IN_ISDIR & event->mask)) {
		remove_dirwatch(event->name, i);
	}

	// TODO move this part of function to a new function like "sync_dir"
	if (!buildpath(pathname, sizeof(pathname), i, NULL, NULL)) {
		return false;
	}

	if (!buildpath(destname, sizeof(destname), i, NULL, option_target)) {
		return false;
	}

	// call rsync to propagate changes in the directory
	if ((IN_CREATE | IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM) & event->mask) {
		printlogf(LOG_NORMAL, "%s of %s in %s --> %s", masktext, event->name, pathname, destname);
		if (!rsync(pathname, destname, false)) {
			// if error on partial rsync, retry with parent dir rsync 
			// TODO once we fix cp -r, MOVE_TO should be fixed also.
			if (dir_watches[i].parent != -1) {
				buildpath(pathname, sizeof(pathname), dir_watches[i].parent, NULL, NULL);
				buildpath(destname, sizeof(destname), dir_watches[i].parent, NULL, option_target);

				printlogf(LOG_NORMAL, "Retry Directory resync with %s to %s", 
									pathname, destname);
				if (!rsync(pathname, destname, true)) {
					printlogf(LOG_ERROR, "Retry of rsync from %s to %s failed", pathname, destname);
					exit(LSYNCD_EXECRSYNCFAIL);
				}
			}
		}
	}

	return 0;
}

/**
 * The control loop waiting for inotify events.
 */
bool master_loop()
{
	char buf[INOTIFY_BUF_LEN];
	int len, i = 0;

	while (keep_going) {
		len = read (inotf, buf, INOTIFY_BUF_LEN);

		if (len < 0) {
			printlogf(LOG_ERROR, "failed to read from inotify (%d:%s)", errno, strerror(errno));
			return false;
		}

		if (len == 0) {
			printlogf(LOG_ERROR, "eof?");
			return false;
		}

		i = 0;

		while (i < len) {
			struct inotify_event *event = (struct inotify_event *) &buf[i];
			handle_event(event);
			i += sizeof(struct inotify_event) + event->len;
		}
	}

	return true;
}

/**
 * Utility function to check file exists. Print out error message and die.
 *
 * @param filename  filename to check
 */
void check_file_exists(const char* filename)
{
	struct stat st;
	if (-1==stat(filename, &st)) {
		printlogf(LOG_ERROR, "File [%s] does not exist\n", filename);
		exit (LSYNCD_FILENOTFOUND);
	}
}


/**
 * Utility function to check given path is absolute path.
 *
 * @param filename  filename to check
 */
void check_absolute_path(const char* filename)
{
	if (filename[0] != '/') {
		printlogf(LOG_ERROR, "Filename [%s] is not an absolute path\n", filename);
		exit (LSYNCD_FILENOTFOUND);
	}
}


/**
 * Prints the help text and exits 0.
 *
 * @param arg0   argv[0] to show what lsyncd was called with.
 */
void print_help(char *arg0)
{
	printf("\n");
	printf("USAGE: %s [OPTION]... SOURCE TARGET\n", arg0);
	printf("\n");
	printf("SOURCE: a directory to watch and rsync.\n");
	printf("\n");
	printf("TARGET: can be any name accepted by rsync. e.g. \"foohost::barmodule/\"\n");
	printf("\n");
	printf("OPTIONS:\n");
	printf("  --debug                Log debug messages\n");
	printf("  --dryrun               Do not call rsync, run dry only\n");
	printf("  --exclude-from FILE    Exclude file handlet to rsync (DEFAULT: None)\n");
	printf("  --help                 Print this help text and exit.\n");
	printf("  --logfile FILE         Put log here (DEFAULT: %s)\n", 
				 logfile);
	printf("  --no-daemon            Do not detach, log to stdout/stderr\n");
	printf("  --rsync-binary FILE    Call this binary to sync (DEFAULT: %s)\n", 
				 rsync_binary);
	printf("  --pidfile FILE         Create a file containing pid of the daemon\n");
	printf("  --scarce               Only log errors\n");
	printf("  --version              Print version an exit.\n");
	printf("\n");
	printf("Take care that lsyncd is allowed to write to the logfile specified.\n");
	printf("\n");
	printf("EXCLUDE FILE: \n");
	printf("  The exclude file may have either filebased general masks like \"*.php\" without directory specifications,\n");
	printf("  or exclude complete directories like \"Data/\". lsyncd will recognize directory excludes by the trailing '/'\n");
	printf("  and will not add watches of directories of exactly such name including sub-directories of them.\n");
	printf("  Please do not try to use more sophisticated exclude masks like \"Data/*.dat\" or \"Da*a/\", \"Data/Volatile/\" etc.\n");
	printf("  This will not work like you would expect it to.\n");
	printf("\n");
	printf("LICENSE\n");
	printf("  GPLv2 or any later version. See COPYING\n");
	printf("\n");
	exit(0);
}

/**
 * Parses the command line options.
 */
bool parse_options(int argc, char **argv)
{

	static struct option long_options[] = {
		{"debug",        0, &loglevel,      1}, 
		{"dryrun",       0, &flag_dryrun,   1}, 
		{"exclude-from", 1, NULL,           0}, 
		{"help",         0, NULL,           0}, 
		{"logfile",      1, NULL,           0}, 
		{"no-daemon",    0, &flag_nodaemon, 1}, 
		{"rsync-binary", 1, NULL,           0}, 
		{"pidfile",      1, NULL,           0}, 
		{"scarce",       0, &loglevel,      3}, 
		{"version",      0, NULL,           0}, 
		{0, 0, 0, 0}
	};

	int c;

	while (1) {
		int oi = 0;
		c = getopt_long_only(argc, argv, "", long_options, &oi);

		if (c == -1) {
			break;
		}

		if (c == '?') {
			return false;
		}

		if (c == 0) { // longoption
			if (!strcmp("help", long_options[oi].name)) {
				print_help(argv[0]);
			}

			if (!strcmp("version", long_options[oi].name)) {
				printf("Version: %s\n", VERSION);
				exit(LSYNCD_SUCCESS);
			}

			if (!strcmp("logfile", long_options[oi].name)) {
				logfile = s_strdup(optarg);
			}

			if (!strcmp("exclude-from", long_options[oi].name)) {
				exclude_file = s_strdup(optarg);
			}

			if (!strcmp("rsync-binary", long_options[oi].name)) {
				rsync_binary = s_strdup(optarg);
			}

			if (!strcmp("pidfile", long_options[oi].name)) {
				pidfile = s_strdup(optarg);
			}

		}
	}


	if (optind + 2 != argc) {
		printf("Error: please specify SOURCE and TARGET (see --help)\n");
		exit(LSYNCD_NOTENOUGHARGUMENTS);
	}

	/* Resolves relative source path, lsyncd might chdir to / later. */
	option_source = realdir(argv[optind]);
	option_target = argv[optind + 1];

	if (!option_source) {
		printf("Error: Source [%s] not found or not a directory.\n", argv[optind]);
		exit(LSYNCD_FILENOTFOUND);
	}

	printlogf(LOG_NORMAL, "syncing %s -> %s\n", option_source, option_target);

	/* sanity checking here */
	if (exclude_file) {
		check_absolute_path(exclude_file);
		check_file_exists(exclude_file);
	}
	if (pidfile) {
		check_absolute_path(pidfile);
	}
	return true;
}

/**
 * Parses the exclude file looking for directory masks to not watch.
 */
bool parse_exclude_file()
{
	FILE * ef;
	char line[PATH_MAX+1];
	int sl;

	ef = fopen(exclude_file, "r");

	if (ef == NULL) {
		printlogf(LOG_ERROR, "Meh, cannot open exclude file '%s'\n", exclude_file);
		exit(LSYNCD_FILENOTFOUND);
	}

	while (1) {
		if (!fgets(line, sizeof(line), ef)) {
			if (feof(ef)) {
				fclose(ef);
				return true;
			}

			printlogf(LOG_ERROR, "Reading file '%s' (%d=%s)\n", exclude_file, errno, strerror(errno));

			exit(LSYNCD_FILENOTFOUND);
		}

		sl = strlen(line);

		if (sl == 0) {
			continue;
		}

		if (line[sl - 1] == '\n') {
			line[sl - 1] = 0;
			sl--;
		}

		if (sl == 0) {
			continue;
		}

		if (line[sl - 1] == '/') {
			if (exclude_dir_n + 1 >= MAX_EXCLUDES) {
				printlogf(LOG_ERROR, "Too many directory excludes, can only have %d at the most", MAX_EXCLUDES);
				exit(LSYNCD_TOOMANYDIRECTORYEXCLUDES);
			}

			line[sl - 1] = 0;

			sl--;

			if (sl == 0) {
				continue;
			}

			printlogf(LOG_NORMAL, "Excluding directories of the name '%s'", line);

			exclude_dirs[exclude_dir_n] = s_malloc(strlen(line) + 1);
			strcpy(exclude_dirs[exclude_dir_n], line);
			exclude_dir_n++;
		}
	}

	return true;
}

void write_pidfile() {
	FILE* f = fopen(pidfile, "w");
	if (!f) {
		printlogf(LOG_ERROR, "Error: cannot write pidfile [%s]\n", pidfile);
		exit(LSYNCD_FILENOTFOUND);
	}
	
	fprintf(f, "%i\n", getpid());
	fclose(f); 
}

/**
 * main
 */
int main(int argc, char **argv)
{
	if (!parse_options(argc, argv)) {
		return -1;
	}

	if (exclude_file) {
		parse_exclude_file();
	}

	inotf = inotify_init();

	if (inotf == -1) {
		printlogf(LOG_ERROR, "Cannot create inotify instance! (%d:%s)", errno, strerror(errno));
		return -1;
	}

	if (!flag_nodaemon) {
		// this will make this process child of init, close stdin/stdout/stderr and chdir to /
		daemon(0, 0);
	}

	printlogf(LOG_NORMAL, "Starting up");

	if (pidfile) {
		write_pidfile();
	}

	dir_watch_size = 2;
	dir_watches = s_calloc(dir_watch_size, sizeof(struct dir_watch));

	printlogf(LOG_NORMAL, "watching %s", option_source);
	add_dirwatch(option_source, "", true, -1);
	if (!rsync(option_source, option_target, true)) {
		printlogf(LOG_ERROR, "Initial rsync from %s to %s failed", option_source, option_target);
		exit(LSYNCD_EXECRSYNCFAIL);
	}

	printlogf(LOG_NORMAL, "--- Entering normal operation with [%d] monitored directories ---", dir_watch_num);

	signal(SIGTERM, catch_alarm);
	master_loop();

	return 0;
}