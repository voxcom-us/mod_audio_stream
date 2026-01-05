export PRIVATE_IP_ADDR=$(hostname -i)

/usr/bin/rtpengine \
    --table=-1 \
    --log-level=4 \
    --interface=private/$PRIVATE_IP_ADDR \
    --interface=public/$PRIVATE_IP_ADDR!$SIP_IP_ADDR \
    --listen-ng=127.0.0.1:2223 \
    --listen-cli=127.0.0.1:60001 \
    --listen-http=127.0.0.1:2225 \
    --timeout=60 \
    --tos=184 \
    --port-min=10000 \
    --port-max=10100

kamailio \
    --log-engine=json:M \
    -u root -g root \
    -P /var/run/kamailio.pid \
    -A "SIP_INTERNAL_SOCKET=udp:$PRIVATE_IP_ADDR:4060" \
    -A "SIP_EXTERNAL_UDP_SOCKET=udp:$PRIVATE_IP_ADDR:5060" \
    -A "SIP_EXTERNAL_TCP_SOCKET=tcp:$PRIVATE_IP_ADDR:5060" \
    -w /etc/kamailio -a no -S -f /etc/kamailio/main.cfg \
    -l "udp:$PRIVATE_IP_ADDR:4060" \
    -l "udp:$PRIVATE_IP_ADDR:5060/$SIP_IP_ADDR:5060" \
    -l "tcp:$PRIVATE_IP_ADDR:5060/$SIP_IP_ADDR:5060" \
    -l "tcp:$PRIVATE_IP_ADDR:8000/$SIP_IP_ADDR:8000" \
    -E -DD
