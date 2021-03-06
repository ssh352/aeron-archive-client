/*
 * Copyright 2018-2019 Fairtide Pte. Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <AeronArchive.h>

namespace aeron {
namespace archive {

struct RecordingData {
    std::int64_t recordingId;
    std::int64_t stopPosition;
    std::int32_t initialTermId;
    std::int64_t termBufferLength;
};

RecordingData getLatestRecordingData(aeron::archive::AeronArchive& archive, const std::string& channel,
                                     std::int32_t streamId);

std::int64_t findLatestRecordingId(aeron::archive::AeronArchive& archive, const std::string& channel,
                                   std::int32_t streamId);

void findAllRecordingIds(aeron::archive::AeronArchive& archive);
}  // namespace archive
}  // namespace aeron
