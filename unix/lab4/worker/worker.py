import os
import signal
import socket
import time

import redis

HOSTNAME = socket.gethostname()
r = redis.Redis(host=os.getenv("REDIS_HOST", "redis"), port=6379, decode_responses=True)
stopping = False


def on_sigterm(signum, frame):
    global stopping
    print(f"[{HOSTNAME}] got signal {signum}, finishing current task and exiting", flush=True)
    stopping = True


signal.signal(signal.SIGTERM, on_sigterm)
signal.signal(signal.SIGINT, on_sigterm)


def factorize(n: int) -> list[int]:
    factors, d = [], 2
    while d * d <= n:
        while n % d == 0:
            factors.append(d)
            n //= d
        d += 1 if d == 2 else 2
    if n > 1:
        factors.append(n)
    return factors


def connect():
    while True:
        try:
            r.ping()
            return
        except redis.ConnectionError:
            print(f"[{HOSTNAME}] waiting for broker...", flush=True)
            time.sleep(1)


def main():
    connect()
    print(f"[{HOSTNAME}] worker started", flush=True)
    while not stopping:
        try:
            item = r.brpop("tasks", timeout=1)
        except redis.ConnectionError:
            time.sleep(1)
            continue
        if item is None:
            continue
        task_id = item[1]
        key = f"task:{task_id}"
        r.hset(key, mapping={"status": "processing", "worker": HOSTNAME})
        n = int(r.hget(key, "n"))
        start = time.time()
        factors = factorize(n)
        r.hset(key, mapping={
            "status": "done",
            "result": " * ".join(map(str, factors)),
            "is_prime": str(len(factors) == 1).lower(),
            "seconds": f"{time.time() - start:.3f}",
            "worker": HOSTNAME,
        })
        r.hincrby("stats:workers", HOSTNAME, 1)
        r.publish(f"done:{task_id}", "done")
        print(f"[{HOSTNAME}] task {task_id} done", flush=True)
    print(f"[{HOSTNAME}] stopped cleanly", flush=True)


if __name__ == "__main__":
    main()
