#! /bin/sh

#### BEGIN INIT INFO
# Provides:          lsyncd
# Required-Start:
# Required-Stop:
# Should-Start:
# Default-Start:
# Default-Stop:
# Short-Description: syncronizes files live on change to a remote machine
# Description:  
#    Lsyncd uses rsync to synchronize local directories with a remote
#    machine running rsyncd. It watches multiple directories trees
#    through inotify. The first step after adding the watches is to
#    rsync all directories with the remote host, and then sync single
#    file by collecting the inotify events. So lsyncd is a light-weight
#	 live mirror solution.
### END INIT INFO

DAEMON=/usr/bin/lsyncd
LSYNCD_ENABLE=false
NAME=lsyncd
USER=daemon
PATH=/sbin:/bin:/usr/sbin:/usr/bin

test -x $DAEMON || exit 0

. /lib/lsb/init-functions

LOGDIR=/var/log/lsyncd
PIDFILE=/var/run/$NAME.pid
DODTIME=2                   # Time to wait for the server to die, in seconds
                            # If this value is set too low you might not
                            # let some servers to die gracefully and
                            # 'restart' will not work

# Include lsyncd defaults if available
if [ -f /etc/default/lsyncd ] ; then
	. /etc/default/lsyncd
fi

set -e

case "$1" in
  start)
	if "$LSYNCD_ENABLE"; then
		log_daemon_msg "Starting lsyncd daemon" "lsyncd"
		if [ -s $PIDFILE ] && kill -0 $(cat $PIDFILE) >/dev/null 2>&1; then
			log_progress_msg "apparently already running"
			log_end_msg 0
			exit 0
		fi
		if start-stop-daemon --start --quiet --user $USER --pidfile $PIDFILE \
			--exec $DAEMON -- --pidfile $PIDFILE $DAEMON_OPTS; then
			rc=0
			sleep 1
			if ! kill -0 $(cat $PIDFILE) >/dev/null 2>&1; then
				log_failure_ms "lsyncd daemon failed to start"
				rc=1
			fi
		else
			rc=1
		fi
		if [ $rc -eq 0 ]; then
			log_end_msg 0
		else
			log_end_msg 1
			rm -f $PIDFILE
		fi
	else
		log_warning_msg "lsyncd daemon not enabled, not starting..."
	fi
	;;
  stop)
	log_daemon_msg "Stopping lsyncd daemon" "lsyncd"
	start-stop-daemon --stop --quiet --oknodo --pidfile $PIDFILE --exec $DAEMON
	log_end_msg $?
	rm -f $PIDFILE
	;;
  restart)
	set +e
	if "$LSYNCD_ENABLE"; then
		log_daemon_msg "Restarting lsyncd daemon" "lsyncd"
		if [ -s $PIDFILE ] && kill -0 $(cat $PIDFILE) >/dev/null 2>&1; then
			start-stop-daemon --stop --quiet --pidfile \
				/var/run/$NAME.pid --exec $DAEMON
		else 
			log_warning_msg "lsyncd daemon not running, attempting to start."
			rm -f $PIDFILE
		fi

		[ -n "$DODTIME" ] && sleep $DODTIME

		if start-stop-daemon --start --quiet --user $USER --pidfile $PIDFILE \
			--exec $DAEMON -- --pidfile $PIDFILE $DAEMON_OPTS; then
			rc=0
			sleep 1
			if ! kill -0 $(cat $PIDFILE) >/dev/null 2>&1; then
				log_failure_ms "lsyncd daemon failed to start"
				rc=1
			fi
		else
			rc=1
		fi
		if [ $rc -eq 0 ]; then
			log_end_msg 0
		else
			log_end_msg 1
			rm -f $PIDFILE
		fi
	else
		log_warning_msg "lsyncd daemon not enabled, not restarting..."
	fi
	;;
  status)
	status_of_proc -p $PIDFILE "$DAEMON" lsyncd && exit 0 || exit $?
	;;
  *)
	N=/etc/init.d/$NAME
	echo "Usage: $N {start|stop|restart|status}" >&2
	exit 1
	;;
esac

exit 0