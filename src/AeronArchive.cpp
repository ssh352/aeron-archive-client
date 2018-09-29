/*
 * Copyright 2018 Fairtide Pte. Ltd.
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


#include <thread>

#include <io_aeron_archive_codecs/ControlResponse.h>

#include "AeronArchive.h"

namespace codecs = io::aeron::archive::codecs;

namespace {

static const std::int32_t FRAGMENT_LIMIT = 10;
static const std::int32_t DEFAULT_RETRY_ATTEMPTS = 3;

}  // namespace

namespace aeron {
namespace archive {

AeronArchive::AeronArchive(const Context& ctx)
    : ctx_(ctx)
    , messageTimeoutNs_(ctx_.messageTimeoutNs()) {
    // TODO: I think it's better to pass a fully prepared context to the constructor
    ctx_.conclude();

    aeron_ = ctx_.aeron();
    // aeronClientInvoker_ = aeron_->conductorAgentInvoker();
    // TODO: get it from the context: idleStrategy_ = ...

    std::int64_t subId = aeron_->addSubscription(ctx_.controlResponseChannel(), ctx_.controlResponseStreamId());
    std::shared_ptr<Subscription> subscription;
    while (!(subscription = aeron_->findSubscription(subId))) {
        std::this_thread::yield();
    }

    controlResponsePoller_ = std::make_unique<ControlResponsePoller>(subscription, FRAGMENT_LIMIT);

    // TODO: Java implementation uses exclusive publication
    std::int64_t pubId = aeron_->addPublication(ctx_.controlRequestChannel(), ctx_.controlRequestStreamId());
    std::shared_ptr<Publication> publication;
    while (!(publication = aeron_->findPublication(pubId))) {
        std::this_thread::yield();
    }

    archiveProxy_ = std::make_unique<ArchiveProxy>(publication, ctx_.messageTimeoutNs(), DEFAULT_RETRY_ATTEMPTS);

    std::int64_t correlationId = aeron_->nextCorrelationId();
    if (!archiveProxy_->connect(ctx_.controlResponseChannel(), ctx_.controlResponseStreamId(), correlationId,
                                aeron_->conductorAgentInvoker())) {
        throw ArchiveException("cannot connect to archive: " + ctx_.controlResponseChannel(), SOURCEINFO);
    }

    controlSessionId_ = awaitSessionOpened(correlationId);
    recordingDescriptorPoller_ =
        std::make_unique<RecordingDescriptorPoller>(subscription, FRAGMENT_LIMIT, controlSessionId_);
}

AeronArchive::AeronArchive(const Context& ctx, const ArchiveProxy& archiveProxy)
    : ctx_(ctx)
    , archiveProxy_(std::make_unique<ArchiveProxy>(archiveProxy)) {
    // TODO
}

std::shared_ptr<AeronArchive> AeronArchive::connect() { return AeronArchive::connect(Context()); }

std::shared_ptr<AeronArchive> AeronArchive::connect(const Context& ctx) { return std::make_shared<AeronArchive>(ctx); }

std::shared_ptr<AeronArchive> AeronArchive::asyncConnect() { return AeronArchive::asyncConnect(Context()); }

std::shared_ptr<AeronArchive> AeronArchive::asyncConnect(const Context& ctx) {
    throw ArchiveException("not implemented", SOURCEINFO);
}

// getters
const Context& AeronArchive::context() const { return ctx_; }

std::int64_t AeronArchive::awaitSessionOpened(std::int64_t correlationId) {
    auto deadline = Clock::now() + messageTimeoutNs_;

    awaitConnection(deadline);

    while (true) {
        pollNextResponse(correlationId, deadline);

        if (controlResponsePoller_->correlationId() != correlationId ||
            controlResponsePoller_->templateId() != codecs::ControlResponse::sbeTemplateId()) {
            aeron_->conductorAgentInvoker().invoke();
            continue;
        }

        auto code = controlResponsePoller_->code();
        if (code != codecs::ControlResponseCode::OK) {
            if (code == codecs::ControlResponseCode::ERROR) {
                throw ArchiveException("unexpected response: " + controlResponsePoller_->errorMessage() +
                                           ", relevant id: " + std::to_string(controlResponsePoller_->relevantId()),
                                       SOURCEINFO);
            }

            throw ArchiveException("unexpected response: code=" + std::to_string(code), SOURCEINFO);
        }

        return controlResponsePoller_->controlSessionId();
    }
}

void AeronArchive::awaitConnection(const TimePoint& deadline) {
    while (!controlResponsePoller_->subscription()->isConnected()) {
        if (Clock::now() > deadline) {
            throw ArchiveException(
                "failed to establish response connection on " + controlResponsePoller_->subscription()->channel() +
                    ", stream id: " + std::to_string(controlResponsePoller_->subscription()->streamId()),
                SOURCEINFO);
        }

        idleStrategy_.idle();
        aeron_->conductorAgentInvoker().invoke();
    }
}

std::int64_t AeronArchive::pollForResponse(std::int64_t correlationId) {
    auto deadline = Clock::now() + messageTimeoutNs_;

    while (true) {
        pollNextResponse(correlationId, deadline);

        if (controlResponsePoller_->controlSessionId() != controlSessionId_ ||
            controlResponsePoller_->templateId() != codecs::ControlResponse::sbeTemplateId()) {
            aeron_->conductorAgentInvoker().invoke();
            continue;
        }

        auto code = controlResponsePoller_->code();
        if (code != codecs::ControlResponseCode::OK) {
            if (code == codecs::ControlResponseCode::ERROR) {
                throw ArchiveException("response for correlation id: " + std::to_string(correlationId) +
                                           ", error: " + controlResponsePoller_->errorMessage() +
                                           ", relevant id: " + std::to_string(controlResponsePoller_->relevantId()),
                                       SOURCEINFO);
            }

            throw ArchiveException("unexpected response: code=" + std::to_string(code), SOURCEINFO);
        }

        if (controlResponsePoller_->correlationId() == correlationId) {
            return controlResponsePoller_->relevantId();
        }
    }
}

void AeronArchive::pollNextResponse(std::int64_t correlationId, const TimePoint& deadline) {
    while (true) {
        std::int32_t fragments = controlResponsePoller_->poll();

        if (controlResponsePoller_->isPollComplete()) {
            break;
        }

        if (fragments > 0) {
            continue;
        }

        if (!controlResponsePoller_->subscription()->isConnected()) {
            throw ArchiveException("subscription to archive is not connected", SOURCEINFO);
        }

        if (Clock::now() > deadline) {
            throw ArchiveException("awaiting response for correlationId=" + std::to_string(correlationId), SOURCEINFO);
        }

        idleStrategy_.idle();
        aeron_->conductorAgentInvoker().invoke();
    }
}

std::int64_t AeronArchive::pollForDescriptors(std::int64_t correlationId, std::int32_t recordCount,
                                              RecordingDescriptorConsumer&& consumer) {
    auto deadline = Clock::now() + messageTimeoutNs_;
    recordingDescriptorPoller_->reset(correlationId, recordCount, std::move(consumer));

    while (true) {
        std::int32_t fragments = recordingDescriptorPoller_->poll();

        if (recordingDescriptorPoller_->isDispatchComplete()) {
            return recordCount - recordingDescriptorPoller_->remainingRecordCount();
        }

        aeron_->conductorAgentInvoker().invoke();

        if (fragments > 0) {
            continue;
        }

        if (!recordingDescriptorPoller_->subscription()->isConnected()) {
            throw ArchiveException("subscription to archive is not connected", SOURCEINFO);
        }

        if (Clock::now() > deadline) {
            throw ArchiveException("awaiting recording descriptors: correlationId=" + std::to_string(correlationId),
                                   SOURCEINFO);
        }

        idleStrategy_.idle();
    }
}

}  // namespace archive
}  // namespace aeron
