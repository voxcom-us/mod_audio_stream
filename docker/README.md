# Docker Compose Workflow

The compose stack builds three services: a Kamailio SIP proxy (`sipproxy`), a FreeSWITCH media node with `mod_audio_stream` (`sipmedia`), and a simple websocket echo server (`echo`).

## Prerequisites

- Docker Engine 24+ with Compose Plugin (or Docker Desktop)
- SignalWire FreeSWITCH apt repository credentials: `FREESWITCH_APT_LOGIN` and `FREESWITCH_APT_PASSWORD`

## Build and Run

```shell
SIP_IP_ADDR=192.168.0.100 \
FREESWITCH_APT_LOGIN=your_login \
FREESWITCH_APT_PASSWORD=your_password \
docker compose -f docker/docker-compose.yml -p audiostream up --build
```

### Open your favorite soft phone and dial

Dial the sip uri sip:1000@192.168.0.100 and you should hear an echo. This is mod_audio_stream playing back audio looped through echo.py

## Useful Commands

- Tail logs: `docker compose -p audiostream logs -f echo`
- Attach to FreeSWITCH CLI: `docker compose -p audiostream exec sipmedia fs_cli`
- Stop and remove containers: `docker compose -p audiostream down`
