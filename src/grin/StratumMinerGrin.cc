/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include "StratumMinerGrin.h"

#include "StratumSessionGrin.h"
#include "StratumServerGrin.h"

#include "DiffController.h"

#include <boost/functional/hash.hpp>

StratumMinerGrin::StratumMinerGrin(
    StratumSessionGrin &session,
    const DiffController &diffController,
    const std::string &clientAgent,
    const std::string &workerName,
    int64_t workerId)
  : StratumMinerBase(
        session, diffController, clientAgent, workerName, workerId) {
}

void StratumMinerGrin::handleRequest(
    const std::string &idStr,
    const std::string &method,
    const JsonNode &jparams,
    const JsonNode &jroot) {
  if (method == "submit") {
    handleRequest_Submit(idStr, jparams);
  }
}

void StratumMinerGrin::handleRequest_Submit(
    const string &idStr, const JsonNode &jparams) {
  // const type cannot access string indexed object member
  JsonNode &jsonParams = const_cast<JsonNode &>(jparams);

  auto &session = getSession();
  if (session.getState() != StratumSession::AUTHENTICATED) {
    session.responseError(idStr, StratumStatus::UNAUTHORIZED);
    return;
  }

  if (jsonParams["edge_bits"].type() != Utilities::JS::type::Int ||
      jsonParams["height"].type() != Utilities::JS::type::Int ||
      jsonParams["job_id"].type() != Utilities::JS::type::Int ||
      jsonParams["nonce"].type() != Utilities::JS::type::Int ||
      jsonParams["pow"].type() != Utilities::JS::type::Array) {
    session.responseError(idStr, StratumStatus::ILLEGAL_PARARMS);
    return;
  }

  uint32_t edgeBits = jsonParams["edge_bits"].uint32();
  uint64_t height = jsonParams["height"].uint32();
  uint32_t prePowHash = jsonParams["job_id"].uint32();
  uint64_t nonce = jsonParams["nonce"].uint64();
  vector<uint64_t> proofs;
  for (auto &p : jsonParams["pow"].array()) {
    if (p.type() != Utilities::JS::type::Int) {
      session.responseError(idStr, StratumStatus::ILLEGAL_PARARMS);
      return;
    } else {
      proofs.push_back(p.uint64());
    }
  }

  auto localJob = session.findLocalJob(prePowHash);
  // can't find local job
  if (localJob == nullptr) {
    session.responseError(idStr, StratumStatus::JOB_NOT_FOUND);
    return;
  }

  auto &server = session.getServer();
  auto &worker = session.getWorker();
  auto sessionId = session.getSessionId();

  shared_ptr<StratumJobEx> exjob = server.GetJobRepository(localJob->chainId_)
                                       ->getStratumJobEx(localJob->jobId_);
  // can't find stratum job
  if (exjob.get() == nullptr) {
    session.responseError(idStr, StratumStatus::JOB_NOT_FOUND);
    return;
  }
  auto sjob = std::static_pointer_cast<StratumJobGrin>(exjob->sjob_);

  auto iter = jobDiffs_.find(localJob);
  if (iter == jobDiffs_.end()) {
    LOG(ERROR) << "can't find session's diff, worker: " << worker.fullName_;
    return;
  }
  auto &jobDiff = iter->second;

  ShareGrin share;
  share.set_version(ShareGrin::CURRENT_VERSION);
  share.set_jobid(sjob->jobId_);
  share.set_workerhashid(workerId_);
  share.set_userid(worker.userId(exjob->chainId_));
  share.set_timestamp((uint64_t)time(nullptr));
  share.set_status(StratumStatus::REJECT_NO_REASON);
  share.set_sharediff(jobDiff.currentJobDiff_);
  share.set_blockdiff(sjob->difficulty_);
  share.set_height(height);
  share.set_nonce(nonce);
  share.set_sessionid(sessionId);
  share.set_edgebits(edgeBits);
  share.set_scaling(
      PowScalingGrin(height, edgeBits, sjob->prePow_.secondaryScaling.value()));
  IpAddress ip;
  ip.fromIpv4Int(session.getClientIp());
  share.set_ip(ip.toString());

  LocalShare localShare(nonce, boost::hash_value(proofs), edgeBits);
  // can't add local share
  if (!localJob->addLocalShare(localShare)) {
    session.responseError(idStr, StratumStatus::DUPLICATE_SHARE);
    // add invalid share to counter
    invalidSharesCounter_.insert((int64_t)time(nullptr), 1);
    return;
  }

  uint256 blockHash;
  server.checkAndUpdateShare(
      localJob->chainId_,
      share,
      exjob,
      proofs,
      jobDiff.jobDiffs_,
      worker.fullName_,
      blockHash);

  if (StratumStatus::isAccepted(share.status())) {
    DLOG(INFO) << "share reached the diff: " << share.scaledShareDiff();
  } else {
    DLOG(INFO) << "share not reached the diff: " << share.scaledShareDiff();
  }

  // we send share to kafka by default, but if there are lots of invalid
  // shares in a short time, we just drop them.
  if (handleShare(idStr, share.status(), share.sharediff())) {
    if (StratumStatus::isSolved(share.status())) {
      server.sendSolvedShare2Kafka(
          localJob->chainId_, share, exjob, proofs, worker, blockHash);
      // mark jobs as stale
      server.GetJobRepository(exjob->chainId_)->markAllJobsAsStale();
    }
  } else {
    // check if there is invalid share spamming
    int64_t invalidSharesNum = invalidSharesCounter_.sum(
        time(nullptr), INVALID_SHARE_SLIDING_WINDOWS_SIZE);
    // too much invalid shares, don't send them to kafka
    if (invalidSharesNum >= INVALID_SHARE_SLIDING_WINDOWS_MAX_LIMIT) {
      LOG(WARNING) << "invalid share spamming, worker: " << worker.fullName_
                   << ", " << share.toString();
      return;
    }
  }

  DLOG(INFO) << share.toString();

  std::string message;
  uint32_t size = 0;
  if (!share.SerializeToArrayWithVersion(message, size)) {
    LOG(ERROR) << "share SerializeToBuffer failed!" << share.toString();
    return;
  }
  server.sendShare2Kafka(localJob->chainId_, message.data(), size);
}
