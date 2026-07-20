#!/bin/bash
# HYTRACK Logging Control Script

# Check if script is run with sudo privileges
if [ "$EUID" -ne 0 ]; then
  echo "Please run with sudo privileges"
  exit 1
fi

# File for passing custom filename
FILENAME_REQUEST_FILE="/tmp/hytrack_filename_request"

# Find HYTRACK PID
HYTRACK_PID=$(pgrep hytrack)

if [ -z "$HYTRACK_PID" ]; then
  echo "HYTRACK is not running."
  exit 1
fi

# Parse command line options
action="status"
custom_filename=""

if [ $# -ge 1 ]; then
  action="$1"
fi

if [ $# -ge 2 ] && [ "$action" = "start" ]; then
  custom_filename="$2"
fi

case "$action" in
  start)
    if [ -z "$custom_filename" ]; then
      echo "Starting logging with auto-generated filename (PID: $HYTRACK_PID)"
    else
      echo "Starting logging with custom filename: $custom_filename (PID: $HYTRACK_PID)"
      
      # Write custom filename to request file
      echo -n "$custom_filename" > "$FILENAME_REQUEST_FILE"
      
      # Set permissions so HYTRACK can read it
      chmod 644 "$FILENAME_REQUEST_FILE"
    fi
    
    # Send the start signal
    kill -SIGUSR2 $HYTRACK_PID
    ;;
    
  stop)
    kill -SIGUSR1 $HYTRACK_PID
    echo "Logging stop signal sent to HYTRACK (PID: $HYTRACK_PID)"
    ;;
    
  status)
    echo "HYTRACK is running with PID: $HYTRACK_PID"
    echo "Use '$0 start [filename]' to start logging or '$0 stop' to stop logging."
    ;;
    
  *)
    echo "Usage: $0 [start|stop|status] [custom_filename]"
    echo "  start [filename]  - Start logging with optional custom filename"
    echo "  stop              - Stop logging"
    echo "  status            - Show HYTRACK status (default)"
    exit 1
    ;;
esac

exit 0