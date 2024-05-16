FROM ubuntu:22.04 as build
WORKDIR /app

# Update the system and install various dependencies
RUN apt-get update && \
    apt-get install -y git cmake ninja-build build-essential tar curl zip unzip pkg-config autoconf python3 \
        libdevil-dev libncurses5-dev libbsd-dev libssl-dev libmysqlclient-dev libboost-system-dev libboost-filesystem-dev
RUN apt-get install -y linux-headers-$(uname -r) && apt-get clean 

# Arm specific
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

# Install vcpkg and the required libraries
RUN git clone https://github.com/Microsoft/vcpkg.git -b 2024.04.26 --depth=1
RUN bash ./vcpkg/bootstrap-vcpkg.sh
RUN ./vcpkg/vcpkg install boost-system cryptopp effolkronium-random libmysql libevent lzo fmt spdlog

COPY . .

# Build the binaries
RUN mkdir -p build
RUN cd build && -DCMAKE_PREFIX_PATH=/app/src/common/ cmake .. -DCMAKE_TOOLCHAIN_FILE=/app/vcpkg/scripts/buildsystems/vcpkg.cmake
RUN make -j $(nproc) install

FROM ubuntu:22.04 as app
WORKDIR /app

RUN apt-get update && apt-get install -y python3 libdevil-dev libbsd-dev && apt-get clean

# Copy the binaries from the build stage
COPY --from=build /app/build/src/db/db /bin/db
COPY --from=build /app/build/src/game/game /bin/game
COPY --from=build /app/build/src/quest/qc /bin/qc

# Copy the game files
COPY ./gamefiles/ .

# Compile the quests
RUN cd /app/locale/english/quest && python3 make.py

# Symlink the configuration files
RUN ln -s "./conf/CMD" "CMD"
RUN ln -s ./conf/item_names_en.txt item_names.txt
RUN ln -s ./conf/item_proto.txt item_proto.txt
RUN ln -s ./conf/mob_names_en.txt mob_names.txt
RUN ln -s ./conf/mob_proto.txt mob_proto.txt
