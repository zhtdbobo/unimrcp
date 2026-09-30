FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential autoconf automake libtool pkg-config ca-certificates \
    libapr1-dev libaprutil1-dev libsofia-sip-ua-dev libcurl4-openssl-dev \
    libjson-c-dev libwebsockets-dev python3 python3-websockets && rm -rf /var/lib/apt/lists/*
# UniMRCP requires the upstream APR pool-mutex extension. Stock distro APR lacks it.
RUN python3 -c "import urllib.request,hashlib; b=urllib.request.urlopen('https://www.unimrcp.org/project/component-view/unimrcp-deps-1-6-0-tar-gz/download',timeout=120).read(); assert hashlib.sha256(b).hexdigest()=='091b0853806bf8742a19209aec2432086969da56970c87804e2168798238ec40'; open('/tmp/deps.tar.gz','wb').write(b)" \
    && tar -xzf /tmp/deps.tar.gz -C /tmp \
    && cd /tmp/unimrcp-deps-1.6.0/libs/apr \
    && ./configure --prefix=/opt/unimrcp-deps --enable-nonportable-atomics \
    && make -j2 && make install \
    && cd ../apr-util && ./configure --prefix=/opt/unimrcp-deps --with-apr=/opt/unimrcp-deps \
    && make -j2 && make install
WORKDIR /src
COPY . .
RUN python3 tests/http-plugins/normalize_source.py && chmod +x bootstrap build/*.sh \
    && ./bootstrap && ./configure --prefix=/opt/unimrcp \
    --with-apr=/opt/unimrcp-deps --with-apr-util=/opt/unimrcp-deps \
    --with-sofia-sip=/usr/lib/x86_64-linux-gnu/pkgconfig/sofia-sip-ua.pc \
    --enable-http-tts-plugin --enable-http-asr-plugin --disable-umc \
    && make -j2 && make install
ENV LD_LIBRARY_PATH=/opt/unimrcp/lib:/opt/unimrcp-deps/lib
CMD ["python3", "tests/http-plugins/test_integration.py"]
