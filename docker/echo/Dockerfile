FROM python:3.12-slim

WORKDIR /opt/echo/app

COPY docker/echo/echo.py .

RUN pip install --no-cache-dir loguru scipy fastapi "uvicorn[standard]"
CMD ["python", "/opt/echo/app/echo.py"]
