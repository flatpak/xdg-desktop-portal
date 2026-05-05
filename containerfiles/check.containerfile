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

# Install git
RUN apt install -y git

# Install pip
RUN apt install -y --no-install-recommends python3-pip

# Install pre-commit
RUN pip install --break-system-packages pre-commit

# Install qdbusxml2cpp
RUN apt install -y --no-install-recommends qt6-base-dev-tools
