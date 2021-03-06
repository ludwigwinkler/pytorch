#include <torch/csrc/distributed/rpc/rref_context.h>
#include <torch/csrc/distributed/rpc/rref_proto.h>

#include <sstream>

namespace torch {
namespace distributed {
namespace rpc {

namespace callback {
void confirmPendingUser(
    const rpc::Message& message,
    const c10::optional<utils::FutureError>& futErr) {
  RRefContext::handleException(futErr);
  auto rr = RemoteRet::fromMessage(message);
  auto& ctx = RRefContext::getInstance();
  ctx.delPendingUser(rr->forkId());
}
} // namespace callback

// Keys for RRef-related debug information.
const std::string kNumOwnerRRefs = "num_owner_rrefs";
const std::string kNumPendingUsers = "num_pending_users";

RRefContext& RRefContext::getInstance() {
  // Leaky singleton to avoid module destructor races.
  static RRefContext* context = new RRefContext(RpcAgent::getCurrentRpcAgent());
  return *context;
}

std::vector<std::shared_ptr<RRef>> RRefContext::destroyInstance(
    bool ignoreRRefLeak) {
  auto& ctx = RRefContext::getInstance();
  {
    std::lock_guard<std::mutex> lock(ctx.destroyedMutex_);
    ctx.destroyed_ = true;
  }
  ctx.checkRRefLeaks(ignoreRRefLeak);
  std::vector<std::shared_ptr<RRef>> deletedRRefs;
  for (auto& entry : ctx.owners_) {
    auto rref = entry.second;
    if (rref->isPyObj()) {
      deletedRRefs.emplace_back(std::move(rref));
    }
  }
  ctx.owners_.clear();
  return deletedRRefs;
}

void RRefContext::handleException(
    const c10::optional<utils::FutureError>& futErr) {
  if (futErr) {
    // TODO: allow users to register an error handler and call it here.
    VLOG(1) << "Got exception: " << (*futErr).what();
    throw std::runtime_error((*futErr).what());
  }
}

RRefContext::RRefContext(std::shared_ptr<RpcAgent> agent)
    : agent_(std::move(agent)), destroyed_(false) {}

RRefContext::~RRefContext() {
  if (!owners_.empty()) {
    VLOG(1) << "Destructing RRefContext with non-empty OwnerRRef set. "
            << "This would likely cause Python deref error. "
            << "Make sure destroyInstance() is invoked before destruction.";
  }
}

std::unordered_map<std::string, std::string> RRefContext::getDebugInfo() {
  std::unordered_map<std::string, std::string> info;
  std::unique_lock<std::mutex> lock(mutex_);
  auto ownerSize = owners_.size();
  auto numPendingUsers = pendingUsers_.size();
  lock.unlock();
  info[kNumOwnerRRefs] = c10::to_string(ownerSize);
  info[kNumPendingUsers] = c10::to_string(numPendingUsers);
  return info;
}

void RRefContext::checkRRefLeaks(bool ignoreRRefLeak) {
  if (!forks_.empty()) {
    std::stringstream ss;
    for (auto& entry : forks_) {
      const RRefId& rrefId = entry.first;
      for (const auto& forkId : entry.second) {
        ss << "Leaking RRef " << rrefId << " with fork Id " << forkId
           << std::endl;
      }
    }

    LOG(WARNING)
        << "Detected RRef Leaks during shutdown. This usually "
        << "occurs when the application code still holds references to RRef "
        << "instances when calling shutdown(). If the program has "
        << "completed correctly and the process is exiting, it is OK to "
        << "ignore these leaks. However, if you program will keep running "
        << "after this, these leaks could result in memory leaks on RRef "
        << "owners. Please make sure all RRefs are out of scope and Python "
        << "GC has deleted them before calling shutdown(): \n"
        << ss.str();
    if (!ignoreRRefLeak) {
      TORCH_CHECK(false, ss.str());
    }
  }
}

std::shared_ptr<UserRRef> RRefContext::createUserRRef(
    worker_id_t ownerId,
    const TypePtr& type) {
  TORCH_CHECK(ownerId != getWorkerId(), "Cannot create UserRRef on owner.");
  // Explicitly creating rrefId before forkId to make sure the order is
  // deterministic, as the argument evaluation order is system and compiler
  // dependent.
  const auto rrefId = genGloballyUniqueId();
  const auto forkId = genGloballyUniqueId();
  return createUserRRef(ownerId, rrefId, forkId, type);
}

std::shared_ptr<UserRRef> RRefContext::createUserRRef(
    worker_id_t ownerId,
    const RRefId& rrefId,
    const ForkId& forkId,
    const TypePtr& type) {
  TORCH_CHECK(ownerId != getWorkerId(), "RRef owner cannot create user RRef.");
  // RRefContext does not track user RRefs, it will be destructed when there
  // is no shared_ptrs pointing to it.
  //
  // NB: cannot use make_shared here as the constructor of UserRRef is private.
  // NB: This UserRRef has not been confirmed by the owner yet. This function's
  // call site is responsible for adding this UserRRef to pendingUsers_.
  // Currently, there are two call sites.
  // (1) The creator user in python_functions.cpp
  // (2) The callee user in RRefContext::notifyOwnerAndParentOfFork.
  //
  // The reason for not adding the pending user here is to put addPendingUser()
  // close to where the RPC occurs, and it is more clear to pair it with
  // deletePendingUser() in the response callback at the call site.
  return std::shared_ptr<UserRRef>(new UserRRef(ownerId, rrefId, forkId, type));
}

void RRefContext::delUser(
    const worker_id_t owner,
    const RRefId& rrefId,
    const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(destroyedMutex_);
  if (!destroyed_) {
    auto fm = agent_->send(
        agent_->getWorkerInfo(owner),
        RRefUserDelete(rrefId, forkId).toMessage());

    fm->addCallback([](const Message& /* unused */,
                       const c10::optional<utils::FutureError>& futErr) {
      RRefContext::handleException(futErr);
    });
  }
}

std::shared_ptr<RRef> RRefContext::getOrCreateRRef(
    const RRefForkData& rrefForkData,
    const TypePtr& type) {
  auto& ownerId = rrefForkData.ownerId_;
  auto& rrefId = rrefForkData.rrefId_;
  auto& forkId = rrefForkData.forkId_;
  if (ownerId == getWorkerId()) {
    auto ownerRRef = getOwnerRRef(rrefId);
    TORCH_INTERNAL_ASSERT(ownerRRef->type() == type);
    return ownerRRef;
  } else {
    return createUserRRef(ownerId, rrefId, forkId, type);
  }
}

std::shared_ptr<OwnerRRef> RRefContext::getOrCreateOwnerRRef(
    const RRefId& rrefId,
    const TypePtr& type) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iter = owners_.find(rrefId);
  if (iter == owners_.end()) {
    // Scenario (1) the first time this owner knows about this RRef
    //
    // NB: cannot use make_shared here as the constructor of OwnerRRef is
    // private.
    auto rref =
        std::shared_ptr<OwnerRRef>(new OwnerRRef(getWorkerId(), rrefId, type));
    owners_[rref->rrefId()] = rref;
    ownerCV_.notify_all();
    return rref;
  } else {
    // Scenario (2) retrieving an existing RRef
    auto ownerRRef = std::static_pointer_cast<OwnerRRef>(iter->second);
    TORCH_INTERNAL_ASSERT(ownerRRef->type() == type);
    return ownerRRef;
  }
}

std::shared_ptr<OwnerRRef> RRefContext::createOwnerRRef(const TypePtr& type) {
  // Don't add this OnwerRRef to the owners_ map yet, otherwise
  // it will never be removed from there. Instead, only add it to the
  // map in prepareChildFork, in case this local RRef is being passed
  // to another worker.
  return std::shared_ptr<OwnerRRef>(
      new OwnerRRef(getWorkerId(), genGloballyUniqueId(), type));
}

std::shared_ptr<OwnerRRef> RRefContext::getOwnerRRef(const RRefId& rrefId) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto iter = owners_.find(rrefId);
  if (iter == owners_.end()) {
    // Scenario (1) RRef is used before it is created
    ownerCV_.wait(lock, [&] { return owners_.find(rrefId) != owners_.end(); });
    return std::static_pointer_cast<OwnerRRef>(owners_[rrefId]);
  } else {
    // Scenario (2) retrieving an existing RRef
    return std::static_pointer_cast<OwnerRRef>(iter->second);
  }
}

RRefForkData RRefContext::prepareChildFork(const std::shared_ptr<RRef>& rref) {
  auto rrefForkData = rref->fork();
  if (rref->isOwner()) {
    // Note [Early Fork Registration]
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // If the parent (caller) is the owner, directly register the fork, instead
    // of waiting for another RREF_FORK_REQUEST or RREF_CHILD_ACCEPT message. An
    // Alternative is adding the fork when the callee user ACKs. However, before
    // that, the owner still have to adds the OwnerRRef into some map to keep it
    // alive (e.g., in pendingChildren_). Hence, adding the fork here or in the
    // ACK does not making any difference but only add complexity.
    // TODO: When adding failure retries and timeout, this fork needs to be
    // deleted if the owner does not receive the ACK within the timeout.
    addForkOfOwner(rrefForkData.rrefId_, rrefForkData.forkId_);
    // ensure that this RRef is in the owners_ list to keep it alive.
    // this is needed for OwnerRRefs that were created locally.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      owners_[rref->rrefId()] = rref;
    }
  } else {
    // Note [Useful Phantom Fork ID for User to Owner Call]
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // If the callee of dist.remote or dist.rpc is the owner of this RRef, the
    // callee will not create a fork using this rrefForkData.forkId_, because
    // the owner will only keep one `OwnerRRef` instance and will not create any
    // `UserRRef` instances. However, this rrefForkData.forkId_ is still
    // necessary, as the caller user needs to keep this `UserRRef` alive until
    // it gets the ACK from the callee owner. Otherwise, the delete message
    // could arrive at the owner before this dist.rpc or dist.remote call, which
    // could potentially trigger the `OwnerRRef` to be deleted before running
    // the user code.
    addPendingChild(rrefForkData.forkId_, rref);
  }
  return rrefForkData;
}

void RRefContext::notifyOwnerAndParentOfFork(
    const ForkId& forkId,
    worker_id_t parent,
    const std::shared_ptr<RRef>& rref) {
  if (parent == rref->owner()) {
    if (parent == agent_->getWorkerInfo().id_) {
      // Owner sending RRef to self, remove the forkId as it was added during
      // pickling
      auto deletedRRef = delForkOfOwner(rref->rrefId(), forkId);
      if (deletedRRef) {
        TORCH_INTERNAL_ASSERT(
            deletedRRef->rrefId() == rref->rrefId(),
            "Deleting a fork of ",
            rref->rrefId(),
            " triggered deleting the OwnerRRef of ",
            deletedRRef->rrefId());
        // NB: not necessary to reset deletedRRef as rref is another shared_ptr
        // instance pointing to the same OwnerRRef.
      }
    } else {
      // If the parent is the owner, this fork has already been added into the
      // forks_ map when the owner sends the message to the callee user. Hence,
      // it is not necessary to send another RREF_CHILD_ACCEPT or
      // RREF_FORK_REQUEST back to the owner. See Note [Early Fork
      // Registration].
    }
    return;
  }

  if (rref->isOwner()) {
    // See Note [Useful Phantom Fork ID for User to Owner Call]
    // In this case, the owner is the caller, and it does not add the fork id
    // into forks_. Because, there will be no real `UserRRef` associated with
    // this fork ID.
    auto fm = agent_->send(
        agent_->getWorkerInfo(parent), RRefChildAccept(forkId).toMessage());
    fm->addCallback([](const Message& /* unused */,
                       const c10::optional<utils::FutureError>& futErr) {
      handleException(futErr);
    });
  } else {
    auto fm = agent_->send(
        agent_->getWorkerInfo(rref->owner()),
        RRefForkRequest(rref->rrefId(), forkId).toMessage());

    addPendingUser(forkId, rref);
    fm->addCallback([this, forkId, parent](
                        const Message& /* unused */,
                        const c10::optional<utils::FutureError>& futErr) {
      handleException(futErr);
      this->finishForkRequest(forkId, parent);
    });
  }
}

void RRefContext::addPendingChild(
    const ForkId& forkId,
    const std::shared_ptr<RRef>& rref) {
  // see Note [Early Fork Registration]
  // If the parent is the owner, it should directly add the child UserRRef as a
  // fork.
  TORCH_INTERNAL_ASSERT(
      !rref->isOwner(), "OwnerRRef should not have a pending child.");
  std::lock_guard<std::mutex> lock(mutex_);
  TORCH_INTERNAL_ASSERT(
      pendingChildren_.find(forkId) == pendingChildren_.end(),
      "Inconsistent states: attempt to add the same child fork twice.");
  pendingChildren_[forkId] = rref;
}

void RRefContext::delPendingChild(const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto iter = pendingChildren_.find(forkId);
  TORCH_INTERNAL_ASSERT(
      iter != pendingChildren_.end(),
      "Inconsistent states: attempt to delete a non-exist child fork.");
  pendingChildren_.erase(iter);
}

void RRefContext::addPendingUser(
    const ForkId& forkId,
    const std::shared_ptr<RRef>& rref) {
  std::lock_guard<std::mutex> lock(mutex_);
  TORCH_INTERNAL_ASSERT(
      pendingUsers_.find(forkId) == pendingUsers_.end(),
      "Inconsistent states: attempt to add the same UserRRef twice.");
  pendingUsers_[forkId] = rref;
}

void RRefContext::delPendingUser(const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto iter = pendingUsers_.find(forkId);
  TORCH_INTERNAL_ASSERT(
      iter != pendingUsers_.end(),
      "Inconsistent states: attempt to delete a non-exist UserRRef.");
  pendingUsers_.erase(iter);
}

void RRefContext::finishForkRequest(const ForkId& forkId, worker_id_t parent) {
  delPendingUser(forkId);
  auto fm = agent_->send(
      agent_->getWorkerInfo(parent), RRefChildAccept(forkId).toMessage());

  fm->addCallback([](const Message& /* unused */,
                     const c10::optional<utils::FutureError>& futErr) {
    handleException(futErr);
  });
}

void RRefContext::addSelfAsFork(std::shared_ptr<OwnerRRef>& rref) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& rrefId = rref->rrefId();
  owners_[rrefId] = rref;
  auto& rrefForks = forks_[rrefId];
  TORCH_INTERNAL_ASSERT(
      rrefForks.find(rrefId) == rrefForks.end(),
      "Attempt to add self as fork twice ",
      rrefId);
  rrefForks.insert(rrefId);
}

void RRefContext::addForkOfOwner(const RRefId& rrefId, const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& rrefForks = forks_[rrefId];
  TORCH_INTERNAL_ASSERT(
      rrefForks.find(forkId) == rrefForks.end(),
      "Got fork notification twice on the same RRef ",
      forkId);
  rrefForks.insert(forkId);
}

std::shared_ptr<RRef> RRefContext::delForkOfOwner(
    const RRefId& rrefId,
    const ForkId& forkId) {
  std::shared_ptr<RRef> deletedRRef = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto rrefIter = forks_.find(rrefId);
    TORCH_INTERNAL_ASSERT(
        rrefIter != forks_.end(),
        "Inconsistent states, deleting a fork before the owner knows it.");
    auto& rrefForks = rrefIter->second;
    auto forkIter = rrefForks.find(forkId);
    TORCH_INTERNAL_ASSERT(
        forkIter != rrefForks.end(),
        "Attempt to delete a non-exist fork ",
        forkId);

    rrefForks.erase(forkId);

    if (rrefForks.empty()) {
      auto ownerIter = owners_.find(rrefId);
      if (ownerIter != owners_.end()) {
        deletedRRef = ownerIter->second;
        owners_.erase(ownerIter);
      }
      forks_.erase(rrefIter);
    }
  }
  return deletedRRef;
}

} // namespace rpc
} // namespace distributed
} // namespace torch
