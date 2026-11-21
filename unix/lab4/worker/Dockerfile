FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY worker.py .
# exec-форма: python становится PID 1 и получает SIGTERM напрямую
ENTRYPOINT ["python", "-u", "worker.py"]
