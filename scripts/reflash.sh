#!/bin/bash
# Kill any existing loggers holding serial ports
pids=$(fuser /dev/ttyACM* 2>/dev/null | tr -s ' ')
if [ -n "$pids" ]; then
    echo "Killing loggers: $pids"
    kill -9 $pids 2>/dev/null
    sleep 1
fi

# Flash all devices
rm -f /tmp/pdn_test/dev*.log
python3 scripts/flash_multi.py --clear-nvs
if [ $? -ne 0 ]; then
    echo "Flash failed, retrying..."
    sleep 2
    python3 scripts/flash_multi.py --clear-nvs
fi

# Start loggers
nohup python3 -u -c "
import serial, threading, signal, time, glob
running = True
def stop(s,f):
    global running; running = False
signal.signal(signal.SIGINT, stop); signal.signal(signal.SIGTERM, stop)
def log_port(port, logfile):
    while running:
        try:
            s = serial.Serial(port, 115200, timeout=1)
            with open(logfile, 'a') as f:
                while running:
                    data = s.read(s.in_waiting or 1)
                    if data: f.write(data.decode('utf-8', errors='replace')); f.flush()
            s.close()
        except: time.sleep(1)
ports = sorted(glob.glob('/dev/ttyACM*'))
for i,p in enumerate(ports):
    threading.Thread(target=log_port, args=(p, f'/tmp/pdn_test/dev{p.split(\"ACM\")[1]}.log'), daemon=True).start()
print(f'Logging {ports}', flush=True)
while running: time.sleep(1)
" > /tmp/pdn_test/logger.out 2>&1 &
echo "Logger PID=$!"
