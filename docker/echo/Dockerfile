FROM python:3.12-slim

WORKDIR /opt/echo/app

COPY docker/echo/echo.py .
COPY docker/echo/ivr-echo_your_audio_back-16k.wav .

RUN pip install --no-cache-dir loguru scipy fastapi "uvicorn[standard]"
CMD ["python", "/opt/echo/app/echo.py"]
