import os
import socket
import time
import uuid

import redis
from fastapi import FastAPI, HTTPException, Query, Response
from pydantic import BaseModel, Field

HOSTNAME = socket.gethostname()
r = redis.Redis(host=os.getenv("REDIS_HOST", "redis"), port=6379, decode_responses=True)
app = FastAPI(title="Factorization service")


class TaskIn(BaseModel):
    n: int = Field(..., ge=2, le=10**18)


@app.post("/tasks", status_code=202)
def create_task(task: TaskIn, response: Response):
    task_id = uuid.uuid4().hex
    r.hset(f"task:{task_id}", mapping={"n": task.n, "status": "queued"})
    r.lpush("tasks", task_id)
    response.headers["X-Served-By"] = HOSTNAME
    return {"id": task_id, "status": "queued", "api": HOSTNAME}


@app.get("/tasks/{task_id}")
def get_task(task_id: str, response: Response):
    data = r.hgetall(f"task:{task_id}")
    if not data:
        raise HTTPException(404, "task not found")
    response.headers["X-Served-By"] = HOSTNAME
    return {"id": task_id, **data, "api": HOSTNAME}


@app.get("/tasks/{task_id}/wait")
def wait_task(task_id: str, response: Response, timeout: int = Query(30, ge=1, le=55)):
    """Long polling: ждёт уведомления от воркера до timeout секунд."""
    key = f"task:{task_id}"
    if not r.exists(key):
        raise HTTPException(404, "task not found")
    pubsub = r.pubsub()
    try:
        pubsub.subscribe(f"done:{task_id}")
        pubsub.get_message(timeout=2)
        data = r.hgetall(key)
        deadline = time.time() + timeout
        while data.get("status") != "done" and time.time() < deadline:
            pubsub.get_message(ignore_subscribe_messages=True, timeout=1.0)
            data = r.hgetall(key)
    finally:
        pubsub.close()
    response.headers["X-Served-By"] = HOSTNAME
    return {"id": task_id, "ready": data.get("status") == "done", **data, "api": HOSTNAME}


@app.get("/stats")
def stats():
    return {
        "api": HOSTNAME,
        "queue_length": r.llen("tasks"),
        "tasks_done_by_worker": r.hgetall("stats:workers"),
    }


@app.get("/health")
def health():
    r.ping()
    return {"status": "ok", "api": HOSTNAME}
