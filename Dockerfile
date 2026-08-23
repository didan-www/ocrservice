FROM eclipse-mosquitto:2.0.20 AS mqtt
USER root
COPY scripts/init-mosquitto.sh /usr/local/bin/init-mosquitto.sh
COPY deploy/mosquitto/entrypoint.sh /usr/local/bin/mosquitto-entrypoint.sh
COPY deploy/mosquitto/mosquitto.conf /mosquitto/config/mosquitto.conf
COPY deploy/mosquitto/acl.base.template /mosquitto/config/acl.base.template
RUN apk add --no-cache bash util-linux su-exec \
    && command -v bash >/dev/null \
    && command -v su-exec >/dev/null \
    && flock -w 0 /tmp/ocrservice-build.lock true \
    && rm -f /tmp/ocrservice-build.lock \
    && chmod 0755 /usr/local/bin/init-mosquitto.sh /usr/local/bin/mosquitto-entrypoint.sh \
    && chown -R mosquitto:mosquitto /mosquitto/config
ENTRYPOINT ["/usr/local/bin/mosquitto-entrypoint.sh"]
CMD ["mosquitto", "-c", "/mosquitto/config/mosquitto.conf"]

FROM --platform=linux/amd64 ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG TARGETPLATFORM=linux/amd64
ARG MYSQL_CONCPP_VERSION=8.4.0
ARG MYSQL_CONCPP_URL=https://dev.mysql.com/get/Downloads/Connector-C++/mysql-connector-c%2B%2B-8.4.0-linux-glibc2.28-x86-64bit.tar.gz
ARG MYSQL_CONCPP_SHA256=0d0ef94f0e20c152b7af5e8c374e915eef96274c1fb4aa936ea855cc989387d3
ARG ONNXRUNTIME_URL=https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-linux-x64-1.20.1.tgz
ARG ONNXRUNTIME_SHA256=67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105

RUN test "${TARGETPLATFORM}" = "linux/amd64" \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config curl ca-certificates git \
        libopencv-dev libssl-dev libcrypt-dev libmysqlclient-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/deps
RUN curl --fail --silent --show-error --location --retry 4 "${MYSQL_CONCPP_URL}" -o connector.tar.gz \
    && printf '%s  %s\n' "${MYSQL_CONCPP_SHA256}" connector.tar.gz | sha256sum -c - \
    && tar -xzf connector.tar.gz \
    && test -d "mysql-connector-c++-${MYSQL_CONCPP_VERSION}-linux-glibc2.28-x86-64bit" \
    && curl --fail --silent --show-error --location --retry 4 "${ONNXRUNTIME_URL}" -o onnxruntime.tgz \
    && printf '%s  %s\n' "${ONNXRUNTIME_SHA256}" onnxruntime.tgz | sha256sum -c - \
    && mkdir -p /tmp/deps/onnxruntime \
    && tar -xzf onnxruntime.tgz --strip-components=1 -C /tmp/deps/onnxruntime \
    && test -f /tmp/deps/onnxruntime/include/onnxruntime_cxx_api.h \
    && test -f /tmp/deps/onnxruntime/lib/libonnxruntime.so

WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DOCRSERVICE_BUILD_TESTING=OFF \
      -DOCRSERVICE_MYSQL_CONCPP_ROOT="/tmp/deps/mysql-connector-c++-${MYSQL_CONCPP_VERSION}-linux-glibc2.28-x86-64bit" \
      -DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME=/tmp/deps/onnxruntime \
      -DCMAKE_CXX_FLAGS="-fstack-protector-strong -D_FORTIFY_SOURCE=2" \
    && cmake --build /build --target ocrservice --parallel

RUN mkdir -p /opt/ocrservice/lib \
    && cp /build/ocrservice /opt/ocrservice/ocrservice \
    && cp "/tmp/deps/mysql-connector-c++-${MYSQL_CONCPP_VERSION}-linux-glibc2.28-x86-64bit/lib64"/*.so* /opt/ocrservice/lib/ \
    && cp -a /tmp/deps/onnxruntime/lib/libonnxruntime*.so* /opt/ocrservice/lib/ \
    && test -s /opt/ocrservice/ocrservice

FROM --platform=linux/amd64 ubuntu:22.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates curl gosu \
        libopencv-core4.5d libopencv-imgcodecs4.5d libopencv-imgproc4.5d \
        libssl3 libgomp1 libcrypt1 libmysqlclient21 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 ocrservice \
    && useradd --system --uid 10001 --gid 10001 --home-dir /nonexistent --shell /usr/sbin/nologin ocrservice

WORKDIR /app
COPY --from=builder /opt/ocrservice/ocrservice /app/ocrservice
COPY --from=builder /opt/ocrservice/lib/ /app/lib/
COPY config/server.json.example /app/config/server.json
COPY migrations/ /app/migrations/
COPY models/ /app/models/
COPY models/LICENSES.md /app/LICENSES.md
COPY deploy/app/entrypoint.sh /usr/local/bin/ocrservice-entrypoint

RUN chmod 0755 /usr/local/bin/ocrservice-entrypoint /app/ocrservice \
    && sed -i 's/"mqttPublicHost": "[^"]*"/"mqttPublicHost": ""/' /app/config/server.json \
    && chmod -R a=rX /app/config /app/migrations /app/models /app/LICENSES.md \
    && mkdir -p /app/data/images /app/data/logs \
    && chown -R ocrservice:ocrservice /app/data \
    && printf '%s  %s\n' bfa426b74d4b619207cca55b296ce3603fb784755a205791af0d0dab4fcf6c04 /app/models/yolov8_plate.onnx | sha256sum -c - \
    && printf '%s  %s\n' c78e54070d0e2b8a6f8d548feb52b75321d5f464bcaadb679ec7ee31433cffa4 /app/models/lprnet.onnx | sha256sum -c - \
    && LD_LIBRARY_PATH=/app/lib ldd /app/ocrservice > /tmp/ocrservice.ldd \
    && ! grep -q 'not found' /tmp/ocrservice.ldd \
    && rm -f /tmp/ocrservice.ldd

ENV LD_LIBRARY_PATH=/app/lib:/usr/local/lib:/usr/lib/x86_64-linux-gnu
EXPOSE 8080
USER root
ENTRYPOINT ["/usr/local/bin/ocrservice-entrypoint"]
CMD ["/app/ocrservice"]
