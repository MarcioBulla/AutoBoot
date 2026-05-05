FROM docker.io/eclipse-mosquitto:2

USER root

RUN apk add --no-cache libcap su-exec \
    && setcap 'cap_net_bind_service=+ep' /usr/sbin/mosquitto

COPY docker-entrypoint.sh /docker-entrypoint.sh

RUN chmod +x /docker-entrypoint.sh

EXPOSE 8883
EXPOSE 443
EXPOSE 80

ENTRYPOINT ["/docker-entrypoint.sh"]
