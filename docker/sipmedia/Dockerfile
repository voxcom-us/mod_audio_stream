# syntax=docker/dockerfile:1.6

FROM debian:bookworm-slim

EXPOSE 8021/tcp
EXPOSE 5060/udp
EXPOSE 5060/tcp
EXPOSE 10000-10010/udp

ARG FREESWITCH_APT_LOGIN
ARG FREESWITCH_APT_PASSWORD

RUN apt update ; \
    apt install ca-certificates \
    gnupg \
    build-essential \
    cmake \
    pkg-config \
    libspeexdsp-dev \
    libssl-dev \
    zlib1g-dev \
    git \
    portaudio19-dev \
    python-is-python3 \
    python3-venv \
    python3-pip \
    vim \
    libodbc2 \
    libtpl0 \
    libjwt0 \
    libmp3lame0 \
    libmpg123-0 \
    libshout3 \
    ssmtp \
    libjpeg-dev \
    libglib2.0-dev \
    libcurl4-openssl-dev \
    libflite1 \
    liblua5.2-0 \
    libsndfile1 \
    libopus0 -qq -y

RUN apt upgrade -qq -y

RUN echo "deb [signed-by=/usr/share/keyrings/signalwire-freeswitch-repo.gpg] https://files.freeswitch.org/repo/deb/debian-release/ bookworm main" > /etc/apt/sources.list.d/freeswitch.list
RUN echo "deb-src [signed-by=/usr/share/keyrings/signalwire-freeswitch-repo.gpg] https://files.freeswitch.org/repo/deb/debian-release/ bookworm main" >> /etc/apt/sources.list.d/freeswitch.list

RUN if [ -z "${FREESWITCH_APT_LOGIN}" ] || [ -z "${FREESWITCH_APT_PASSWORD}" ]; then \
      echo "FREESWITCH_APT_LOGIN and FREESWITCH_APT_PASSWORD must be set" >&2; \
      exit 1; \
    fi; \
    printf 'machine files.freeswitch.org\nlogin %s\npassword %s\n' "${FREESWITCH_APT_LOGIN}" "${FREESWITCH_APT_PASSWORD}" > /etc/apt/auth.conf; \
    chmod 600 /etc/apt/auth.conf

ADD docker/sipmedia/usr/share/keyrings/signalwire-freeswitch-repo.gpg /usr/share/keyrings/signalwire-freeswitch-repo.gpg

RUN apt update ; apt install python-esl gdb wget libevent-dev libfreeswitch-dev libfreeswitch1 libfreeswitch1-dbg freeswitch-dbg freeswitch-mod-xml-curl freeswitch-mod-xml-curl-dbg freeswitch-mod-sofia freeswitch-mod-sofia-dbg freeswitch-mod-shout freeswitch-mod-shout-dbg freeswitch-mod-say-en freeswitch-mod-say-en-dbg freeswitch-mod-rtc freeswitch-mod-rtc-dbg freeswitch-mod-opus freeswitch-mod-opus-dbg freeswitch-mod-httapi freeswitch-mod-httapi-dbg freeswitch-mod-event-socket freeswitch-mod-event-socket-dbg freeswitch-mod-dptools freeswitch-mod-dptools-dbg freeswitch-mod-distributor freeswitch-mod-distributor-dbg freeswitch-mod-directory freeswitch-mod-directory-dbg freeswitch-mod-dialplan-xml freeswitch-mod-dialplan-xml-dbg freeswitch-mod-console freeswitch-mod-console-dbg freeswitch-mod-conference freeswitch-mod-conference-dbg freeswitch-mod-commands freeswitch-mod-commands-dbg freeswitch-mod-av freeswitch-mod-av-dbg freeswitch-mod-hash freeswitch-mod-hash-dbg freeswitch-mod-sndfile freeswitch-mod-sndfile-dbg freeswitch-mod-native-file freeswitch-mod-native-file-dbg freeswitch-mod-tone-stream freeswitch-mod-tone-stream-dbg freeswitch-mod-db freeswitch-mod-db-dbg freeswitch-mod-local-stream freeswitch-mod-local-stream-dbg freeswitch-mod-lua freeswitch-mod-lua-dbg freeswitch-mod-flite freeswitch-mod-flite-dbg freeswitch-mod-callcenter freeswitch-mod-callcenter-dbg freeswitch-mod-json-cdr freeswitch-mod-json-cdr-dbg freeswitch-mod-png freeswitch-mod-png-dbg freeswitch-mod-logfile freeswitch-mod-logfile-dbg freeswitch-mod-loopback freeswitch-mod-loopback-dbg freeswitch-mod-verto freeswitch-mod-verto-dbg freeswitch-mod-curl freeswitch-mod-curl-dbg freeswitch-mod-expr freeswitch-mod-expr-dbg freeswitch-mod-fifo freeswitch-mod-fifo-dbg freeswitch-mod-voicemail freeswitch-mod-voicemail-dbg freeswitch-mod-esf freeswitch-mod-esf-dbg freeswitch-mod-fsv freeswitch-mod-fsv-dbg freeswitch-mod-valet-parking freeswitch-mod-valet-parking-dbg freeswitch-mod-spandsp freeswitch-mod-spandsp-dbg freeswitch-mod-g723-1 freeswitch-mod-g723-1-dbg freeswitch-mod-avmd freeswitch-mod-avmd-dbg freeswitch-mod-amr freeswitch-mod-amr-dbg freeswitch-mod-b64 freeswitch-mod-b64-dbg freeswitch-lang-en freeswitch-systemd freeswitch -qq -y

ADD docker/sipmedia/etc/fs_cli.conf /etc/fs_cli.conf
ADD docker/sipmedia/etc/freeswitch/vars.xml /etc/freeswitch/vars.xml
ADD docker/sipmedia/etc/freeswitch/mime.types /etc/freeswitch/mime.types
ADD docker/sipmedia/etc/freeswitch/freeswitch.xml /etc/freeswitch/freeswitch.xml

COPY . /mod_audio_stream
RUN cd /mod_audio_stream ; git submodule init ; git submodule update; rm -rf build; mkdir -p build ; cd build ; cmake -DUSE_TLS=ON -DCMAKE_BUILD_TYPE=Release .. ; make install
