# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

FROM gcr.io/oss-fuzz-base/base-builder as builder
RUN apt-get update && apt install -y autoconf automake libssl-dev libcunit1 libcunit1-dev bison libbison-dev

COPY . cyclonedds

COPY mayhem/build.sh $SRC
WORKDIR $SRC/cyclonedds
ENV FUZZING_LANGUAGE=c SANITIZER=address
RUN compile
WORKDIR /out
RUN for zip in $(ls *.zip); do unzip $zip -d $(basename $zip .zip); done

FROM ubuntu

COPY --from=builder /out /
