################################################################################
# Dockerfile: Ubuntu 24.04 LTS (Noble Numbat)
# - Multi-stage Production Build for Motion Vector DVR Cutter
# - FFmpeg 8.0, fmtlog, jemalloc, clang-21
################################################################################

############################ STAGE : 1 - The Builder ############################
FROM ubuntu:24.04 AS builder

# 1) Set frontend to noninteractive and enable all necessary repos
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    software-properties-common wget gnupg ca-certificates && \
    add-apt-repository main && \
    add-apt-repository universe && \
    add-apt-repository restricted && \
    add-apt-repository multiverse

# 2) Install all necessary build dependencies (clang-18 is native to Ubuntu 24.04)
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git pkg-config libtool autoconf automake \
    ninja-build yasm nasm curl dpkg-dev \
    clang-18 lld \
    libx264-dev libx265-dev libvpx-dev libfdk-aac-dev libmp3lame-dev \
    libvorbis-dev libopus-dev libopenjp2-7-dev zlib1g-dev libssl-dev \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# 3) Set clang-18 as default compiler
RUN update-alternatives --install /usr/bin/cc cc /usr/bin/clang-18 100 && \
    update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-18 100
ENV CC=clang-18
ENV CXX=clang++-18

ENV FFMPEG_CFLAGS="-O3 -march=native -fstack-protector-strong -fPIE"
ENV FFMPEG_LDFLAGS="-Wl,-O1 -Wl,--as-needed -Wl,-z,relro,-z,now -fuse-ld=lld"

# 5) Build jemalloc from source
RUN git clone --depth 1 --branch 5.3.0 https://github.com/jemalloc/jemalloc.git /tmp/jemalloc && \
    cd /tmp/jemalloc && \
    ./autogen.sh && \
    ./configure CC=${CC} CXX=${CXX} --prefix=/usr/local && \
    make -j$(nproc) && make install && \
    ldconfig && rm -rf /tmp/jemalloc

# 8) Build FFmpeg 8.0 from source
RUN wget -q https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz -O /tmp/ffmpeg.tar.xz && \
    tar -xJf /tmp/ffmpeg.tar.xz -C /tmp && cd /tmp/ffmpeg-8.0 && \
    ./configure \
        --cc=${CC} --cxx=${CXX} --prefix=/usr/local \
        --enable-gpl --enable-nonfree \
        --enable-version3 \
        --enable-libx264 --enable-libx265 --enable-libvpx \
        --enable-libfdk-aac --enable-libmp3lame --enable-libvorbis --enable-libopus --enable-libopenjpeg \
        --enable-openssl \
        --enable-hwaccels \
        --enable-lto \
        --enable-gray \
        --disable-debug \
        --enable-asm \
        --enable-pic --enable-shared \
        --disable-doc --disable-htmlpages --disable-manpages --disable-podpages \
        --extra-cflags="${FFMPEG_CFLAGS}" \
        --extra-ldflags="${FFMPEG_LDFLAGS}" && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/ffmpeg* && ldconfig

# 6) Build fmt from source
RUN git clone --depth 1 --branch 11.0.0 https://github.com/fmtlib/fmt.git /tmp/fmt && \
    cd /tmp/fmt && cmake -B build -DCMAKE_BUILD_TYPE=Release -DFMT_INSTALL=ON && \
    cmake --build build -j$(nproc) && cmake --install build && rm -rf /tmp/fmt

# 9) Copy and compile the Motion Cut project
WORKDIR /src
COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/
COPY tools/ tools/
COPY config/ config/

RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-18 \
    -DCMAKE_CXX_COMPILER=clang++-18 && \
    cmake --build build -j$(nproc)

############################ STAGE : 2 - The Final Image ############################
FROM ubuntu:24.04

# 1) Enable multiverse for runtime libs and install dependencies
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common && \
    add-apt-repository multiverse && \
    apt-get update && apt-get install -y --no-install-recommends \
    libatomic1 libvpx9 libx264-164 libx265-199 libfdk-aac2 \
    libmp3lame0 libvorbis0a libvorbisenc2 libopus0 libopenjp2-7 ca-certificates \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# 2) Copy necessary assets from the builder stage
COPY --from=builder /usr/local/lib/ /usr/local/lib/
COPY --from=builder /usr/local/bin/ffmpeg /usr/local/bin/ffmpeg
COPY --from=builder /src/build/motion_trim /usr/local/bin/motion_trim

# 3) Create a directory for the configuration file with env & copy it
RUN mkdir -p /etc/mc/configs
COPY config/motion_trim.env /etc/mc/configs/motion_trim.env

# 4) Set up the runtime environment
RUN ldconfig
ENV LD_PRELOAD=/usr/local/lib/libjemalloc.so
WORKDIR /

# 5) Define the default command
# For development: sleep infinity to keep container running for exec
CMD ["sleep", "infinity"]
