FROM debian:trixie-slim

EXPOSE 8000/tcp
EXPOSE 5060/udp
EXPOSE 5060/tcp
EXPOSE 4060/udp
EXPOSE 10000-10100/udp

RUN apt-get update -qq && apt-get install wget gnupg sqlite3 grc vim procps dnsutils sngrep python-is-python3 python3-pip python3-venv -y

# install kamailio
RUN wget https://deb.kamailio.org/kamailiodebkey.gpg -O /etc/apt/trusted.gpg.d/kamailio.asc
RUN echo "deb [signed-by=/etc/apt/trusted.gpg.d/kamailio.asc] http://deb.kamailio.org/kamailio60 trixie main" > /etc/apt/sources.list.d/kamailio.list
RUN echo "deb-src [signed-by=/etc/apt/trusted.gpg.d/kamailio.asc] http://deb.kamailio.org/kamailio60 trixie main" >> /etc/apt/sources.list.d/kamailio.list
RUN apt-get update -qq && apt-get install kamailio kamailio-sqlite-modules kamailio-utils-modules kamailio-json-modules kamailio-outbound-modules kamailio-extra-modules -y

# Create architecture-independent symlink for kamailio modules
RUN mkdir -p /usr/lib/kamailio && \
    ln -sf /usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)/kamailio/modules /usr/lib/kamailio/modules

# install rtpengine
RUN wget https://rtpengine.dfx.at/latest/pool/main/r/rtpengine-dfx-repo-keyring/rtpengine-dfx-repo-keyring_1.0_all.deb
RUN dpkg -i rtpengine-dfx-repo-keyring_1.0_all.deb
RUN echo "deb [signed-by=/usr/share/keyrings/dfx.at-rtpengine-archive-keyring.gpg] https://rtpengine.dfx.at/12.3 trixie main" > /etc/apt/sources.list.d/rtpengine.list
RUN apt-get update -qq && apt-get install -qq rtpengine

# setup kamailio
ADD docker/sipproxy/etc/kamailio/sqlite.sql /etc/kamailio/sqlite.sql
RUN sqlite3 /etc/kamailio/kamailio.db < /etc/kamailio/sqlite.sql
ADD docker/sipproxy/etc/kamailio/main.cfg /etc/kamailio/main.cfg
ADD docker/sipproxy/etc/kamailio/dispatcher.list /etc/kamailio/dispatcher.list

ADD docker/sipproxy/start.sh /etc/kamailio/start.sh
