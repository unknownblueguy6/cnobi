#!/bin/sh

# Copyright 2011 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

SYSTEMNAME=`uname -s`

cat >config.ninja <<EOT
# This file is generated by bootstrap.sh.
EOT

if [ "${SYSTEMNAME}" = "Linux" ]; then
cat >>config.ninja <<EOT
conf_cflags = -O2
conf_ldflags = -s
EOT
elif [ "${SYSTEMNAME}" = "FreeBSD" ]; then
cat >>config.ninja <<EOT
conf_cflags = -O2 -I/usr/local/include
conf_ldflags = -s -L/usr/local/lib -lexecinfo
EOT
fi

cat >>config.ninja <<EOT
# When developing:
# conf_cflags = -g -Wall
# conf_ldlags =
EOT

echo "Building ninja manually..."
srcs=$(ls src/*.cc | grep -v test)
if [ "${SYSTEMNAME}" = "Linux" ]; then
    g++ -Wno-deprecated -o ninja.bootstrap $srcs
elif [ "${SYSTEMNAME}" = "FreeBSD" ]; then
    g++ -Wno-deprecated -o ninja.bootstrap -I/usr/local/include -L/usr/local/lib -lexecinfo $srcs
fi

echo "Building ninja using itself..."
./ninja.bootstrap ninja
rm ninja.bootstrap

echo "Done!"
