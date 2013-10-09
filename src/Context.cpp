 /** simple galois context and contention manager -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */
#include "Galois/Runtime/Context.h"
#include "Galois/Runtime/MethodFlags.h"
#include "Galois/Runtime/ll/SimpleLock.h"
#include "Galois/Runtime/ll/CacheLineStorage.h"

//! Global thread context for each active thread
static __thread Galois::Runtime::SimpleRuntimeContext* thread_ctx = 0;

namespace {

struct PendingStatus {
  Galois::Runtime::LL::CacheLineStorage<Galois::Runtime::PendingFlag> flag;
  PendingStatus(): flag(Galois::Runtime::NON_DET) { }
};

PendingStatus pendingStatus;

}

void Galois::Runtime::setPending(Galois::Runtime::PendingFlag value) {
  pendingStatus.flag.data = value;
}

Galois::Runtime::PendingFlag Galois::Runtime::getPending() {
  return pendingStatus.flag.data;
}

void Galois::Runtime::doCheckWrite() {
  if (getPending () == PENDING) {
    throw failsafe_ex();
  }
}

void Galois::Runtime::setThreadContext(Galois::Runtime::SimpleRuntimeContext* ctx) {
  thread_ctx = ctx;
}

Galois::Runtime::SimpleRuntimeContext* Galois::Runtime::getThreadContext() {
  return thread_ctx;
}



void Galois::Runtime::signalConflict(Lockable* lockable) {
  throw conflict_ex{lockable}; // Conflict
}

void Galois::Runtime::forceAbort() {
  signalConflict(NULL);
}

////////////////////////////////////////////////////////////////////////////////
// LockManagerBase & SimpleRuntimeContext
////////////////////////////////////////////////////////////////////////////////

#if !defined(GALOIS_USE_SEQ_ONLY)

Galois::Runtime::LockManagerBase::AcquireStatus Galois::Runtime::LockManagerBase::tryAcquire (Galois::Runtime::Lockable* lockable) {
  assert(lockable);
  if (tryLock (lockable)) {
    assert(!getOwner (lockable));
    ownByForce (lockable);
    return NEW_OWNER;
  } else if (getOwner (lockable) == this) {
    return ALREADY_OWNER;
  }
  return FAIL;
}

void Galois::Runtime::SimpleRuntimeContext::acquire(Galois::Runtime::Lockable* lockable) {
  AcquireStatus i;
  if (customAcquire) {
    subAcquire(lockable);
  } else if ((i = tryAcquire(lockable)) != AcquireStatus::FAIL) {
    if (i == AcquireStatus::NEW_OWNER) {
      addToNhood (lockable);
    }
  } else {
    Galois::Runtime::signalConflict(lockable);
  }
}

void Galois::Runtime::SimpleRuntimeContext::release(Galois::Runtime::Lockable* lockable) {
  assert(lockable);
  // The deterministic executor, for instance, steals locks from other
  // iterations
  assert(customAcquire || getOwner(lockable) == this);
  assert(!lockable->next);
  lockable->owner.unlock_and_clear();
}

unsigned Galois::Runtime::SimpleRuntimeContext::commitIteration() {
  unsigned numLocks = 0;
  while (locks) {
    //ORDER MATTERS!
    Lockable* lockable = locks;
    locks = lockable->next;
    lockable->next = 0;
    LL::compilerBarrier();
    release(lockable);
    ++numLocks;
  }

  return numLocks;
}

unsigned Galois::Runtime::SimpleRuntimeContext::cancelIteration() {
  return commitIteration();
}
#endif

void Galois::Runtime::SimpleRuntimeContext::subAcquire(Galois::Runtime::Lockable* lockable) {
  GALOIS_DIE("Shouldn't get here");
}

