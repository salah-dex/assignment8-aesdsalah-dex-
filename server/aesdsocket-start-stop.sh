#!/bin/sh

### BEGIN INIT INFO
# Provides:          aesdsocket
# Required-Start:    $network
# Required-Stop:     $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Start aesdsocket daemon
### END INIT INFO

DAEMON=/usr/bin/aesdsocket
DAEMON_NAME=aesdsocket
PIDFILE=/var/run/aesdsocket.pid

case "$1" in
  start)
    echo "Starting $DAEMON_NAME"
    start-stop-daemon -S -n $DAEMON_NAME -a  $DAEMON -- -d
    ;;
  stop)
    echo "Stopping $DAEMON_NAME"
    start-stop-daemon -K -n $DAEMON_NAME -s SIGTERM
    ;;
  restart)
    $0 stop
    sleep 1
    $0 start
    ;;
  *)
    echo "Usage: $0 {start|stop|restart}"
    exit 1
    ;;
esac

exit 0
