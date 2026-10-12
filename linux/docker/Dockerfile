#
# Notepad++ GTK port — containerised build, test and packaging.
# Build context is the REPOSITORY ROOT (the port needs ../scintilla etc.).
#
#   # .deb into ./dist
#   docker build -f linux/docker/Dockerfile --target artifact --output type=local,dest=dist .
#
#   # runnable image with the .deb installed (default target)
#   docker build -f linux/docker/Dockerfile -t notepad-plus-plus-gtk .
#
#   # toolchain-only image for iterative builds with the source bind-mounted
#   docker build -f linux/docker/Dockerfile --target toolchain -t npp-gtk-toolchain .
#
# See linux/docs/BUILD.md for details (X11/Wayland, proxies, other releases).

ARG UBUNTU_VERSION=24.04

# ---------------------------------------------------------------------------
# toolchain: compilers + GTK headers + test/packaging tools
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS toolchain
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config \
        libgtk-3-dev \
        dpkg-dev file \
        xvfb xauth \
        ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src

# ---------------------------------------------------------------------------
# build: configure, compile, run the headless test suite, make the .deb
# ---------------------------------------------------------------------------
FROM toolchain AS build
ARG BUILD_TYPE=Release
ARG DEB_REVISION=1
ARG DEB_MAINTAINER="Sergii Shcherbakov <shchers@gmail.com>"
COPY . /src
RUN cmake -S linux -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DNPP_DEB_REVISION=${DEB_REVISION} \
        -DCPACK_PACKAGE_CONTACT="${DEB_MAINTAINER}" \
        -DNPP_WARNINGS_AS_ERRORS=ON \
 && cmake --build /build \
 && ctest --test-dir /build --output-on-failure \
 && cpack --config /build/CPackConfig.cmake -B /build/pkg \
 && rm -rf /build/pkg/_CPack_Packages

# ---------------------------------------------------------------------------
# artifact: just the .deb, for `docker build --output`
# ---------------------------------------------------------------------------
FROM scratch AS artifact
COPY --from=build /build/pkg/*.deb /

# ---------------------------------------------------------------------------
# runtime (default): clean Ubuntu with the package installed. Installing here
# also proves the .deb's Depends: are complete on a bare system.
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime
ARG DEBIAN_FRONTEND=noninteractive
COPY --from=build /build/pkg/*.deb /tmp/pkg/
RUN apt-get update \
 && apt-get install -y --no-install-recommends /tmp/pkg/*.deb \
        adwaita-icon-theme fonts-dejavu-core librsvg2-common \
 && rm -rf /var/lib/apt/lists/* /tmp/pkg \
 && notepad-plus-plus-gtk --version \
 && useradd --create-home --shell /bin/bash npp
USER npp
WORKDIR /home/npp
ENV NO_AT_BRIDGE=1
ENTRYPOINT ["notepad-plus-plus-gtk"]
