import signal,time
signal.signal(signal.SIGINT, signal.SIG_IGN)
s = "c" * 1024 * 1024 * 300
time.sleep(300)
