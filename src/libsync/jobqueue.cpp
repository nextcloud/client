/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "jobqueue.h"

#include "abstractnetworkjob.h"

#include <QLoggingCategory>

namespace {
const int MaxRetryCount = 5;
}


namespace OCC {

Q_LOGGING_CATEGORY(lcJobQUeue, "sync.networkjob.jobqueue", QtDebugMsg)

void JobQueue::setBlocked(bool block)
{
    if (block) {
        _blocked++;
    } else {
        _blocked--;
    }
    if (_blocked == 0) {
        auto tmp = std::move(_jobs);
        for (auto job : tmp) {
            if (job) {
                qCDebug(lcJobQUeue) << "Retry" << job << job->path();
                job->retry();
            }
        }
    }
}

bool JobQueue::isBlocked() const
{
    return _blocked;
}

bool JobQueue::retry(AbstractNetworkJob *job)
{
    if (!needsRetry(job)) {
        return false;
    }
    if (_blocked) {
        qCDebug(lcJobQUeue) << "Retry queued" << job << job->url();
        _jobs.push_back(job);
    } else {
        qCDebug(lcJobQUeue) << "Direct retry" << job << job->url();
        job->retry();
    }
    return true;
}

bool JobQueue::enqueue(AbstractNetworkJob *job)
{
    if (!_blocked) {
        return false;
    }
    qCDebug(lcJobQUeue) << "Queue" << job << job->url();
    _jobs.push_back(job);
    return true;
}

void JobQueue::clear()
{
    _blocked = false;
    auto tmp = std::move(_jobs);
    for (auto job : tmp) {
        if (job) {
            qCDebug(lcJobQUeue) << "Abort" << job << job->path();
            job->abort();
        }
    }
}

size_t JobQueue::size() const
{
    return _jobs.size();
}

bool JobQueue::needsRetry(AbstractNetworkJob *job) const
{
    if (job->isAuthenticationJob()) {
        qCDebug(lcJobQUeue) << "Not Retry auth job" << job << job->url();
        return false;
    }
    if (job->retryCount() >= MaxRetryCount) {
        qCDebug(lcJobQUeue) << "Not Retry too many retries" << job->retryCount() << job << job->url();
        return false;
    }

    if (auto reply = job->reply()) {
        if (!reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isNull()) {
            return true;
        }
        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() == QNetworkReply::AuthenticationRequiredError) {
                return true;
            }
        }
    }
    return false;
}

}
