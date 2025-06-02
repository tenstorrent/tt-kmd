FROM alpine:latest
RUN apk add akms
WORKDIR /usr/src/tenstorrent/
COPY . /usr/src/tenstorrent/
RUN akms install .
CMD ["modprobe", "tenstorrent"]