# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

# This Containerfile builds the image that we use to check the project.
# When this file is changed, or one needs to rebuild the image for another
# reason, bump the `image` tag in the main.yml workflow.

FROM ubuntu:26.04

RUN apt update
RUN apt upgrade -y

# Set locale
RUN apt install -y locales
RUN sed -i -e 's/# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen && \
    dpkg-reconfigure --frontend=noninteractive locales && \
    update-locale LANG=en_US.UTF-8
ENV LANG=en_US.UTF-8

# Install dependencies
RUN apt install -y git cargo python3-pip

# Install pre-commit and gitlint
RUN pip install --break-system-packages pre-commit gitlint-core==0.19.1

# Install qdbusxml2cpp
RUN apt install -y --no-install-recommends qt6-base-dev-tools

# Install our potfile check script
COPY containerfiles/check-potfiles.sh /usr/local/bin

# Install gobject-linter
RUN cargo install --root /usr/local \
                  --git https://github.com/bilelmoussaoui/gobject-linter \
                  --rev 0622b96 \
                  gobject-linter

# gitlint needs a git identity to function
RUN git config --global user.name "John Doe"
RUN git config --global user.email "john.doe@example.org"

# Bake in the checkouts of the config
ENV PRE_COMMIT_HOME=/pre-commit-home
COPY containerfiles/pre-commit-config.yaml /etc/pre-commit-config.yaml
WORKDIR /pre-commit
COPY containerfiles/pre-commit-config.yaml ./.pre-commit-config.yaml
RUN git init . && \
    git add --all && \
    pre-commit install-hooks
WORKDIR /
RUN rm -rf /pre-commit

# Set the final workdir and entrypoint
WORKDIR /workspace
ENTRYPOINT ["pre-commit", "run", "-c", "/etc/pre-commit-config.yaml"]
