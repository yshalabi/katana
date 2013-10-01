/** Galois scheduler and runtime -*- C++ -*-
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
 * Implementation of the Galois foreach iterator. Includes various 
 * specializations to operators to reduce runtime overhead.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */
#ifndef GALOIS_RUNTIME_PARALLELWORK_H
#define GALOIS_RUNTIME_PARALLELWORK_H

#include "Galois/Mem.h"
#include "Galois/Statistic.h"
#include "Galois/Runtime/Barrier.h"
#include "Galois/Runtime/Context.h"
#include "Galois/Runtime/ForEachTraits.h"
#include "Galois/Runtime/Range.h"
#include "Galois/Runtime/Support.h"
#include "Galois/Runtime/Termination.h"
#include "Galois/Runtime/ThreadPool.h"
#include "Galois/Runtime/UserContextAccess.h"
#include "Galois/WorkList/GFifo.h"

#include <algorithm>
#include <functional>

#ifdef GALOIS_USE_HTM
#include <speculation.h>
#endif

namespace Galois {
//! Internal Galois functionality - Use at your own risk.
namespace Runtime {
namespace {

template<bool Enabled> 
class LoopStatistics {
  unsigned long conflicts;
  unsigned long iterations;
  const char* loopname;

#ifdef GALOIS_USE_HTM
  TmReport_s start;
  void init() { 
    if (LL::getTID()) return;

    // Dummy transaction to ensure that tm_get_all_stats doesn't return
    // garbage 
#pragma tm_atomic
    {
      conflicts = 0;
    }

    tm_get_all_stats(&start);
  }

  void report() { 
    if (LL::getTID()) return;
    TmReport_s stop;
    tm_get_all_stats(&stop);
    reportStat(loopname, "HTMTransactions", 
        stop.totalTransactions - start.totalTransactions);
    reportStat(loopname, "HTMRollbacks", 
        stop.totalRollbacks - start.totalRollbacks);
    reportStat(loopname, "HTMSerializedJMV", 
        stop.totalSerializedJMV - start.totalSerializedJMV);
    reportStat(loopname, "HTMSerializedMAXRB", 
        stop.totalSerializedMAXRB - start.totalSerializedMAXRB);
    reportStat(loopname, "HTMSerializedOTHER", 
        stop.totalSerializedOTHER - start.totalSerializedOTHER);
    tm_print_stats();
  }
#else
  void init() { }
  void report() { }
#endif

public:
  explicit LoopStatistics(const char* ln) :conflicts(0), iterations(0), loopname(ln) { init(); }
  ~LoopStatistics() {
    reportStat(loopname, "Conflicts", conflicts);
    reportStat(loopname, "Iterations", iterations);
    report();
  }
  inline void inc_iterations(int amount = 1) {
    iterations += amount;
  }
  inline void inc_conflicts() {
    ++conflicts;
  }
};


template <>
class LoopStatistics<false> {
public:
  explicit LoopStatistics(const char* ln) {}
  inline void inc_iterations(int amount = 1) const { }
  inline void inc_conflicts() const { }
};

template<typename value_type>
class AbortHandler {
  struct Item { value_type val; int retries; };

  typedef WorkList::GFIFO<Item> AbortedList;
  PerThreadStorage<AbortedList> queues;

  /**
   * Policy: retry work 2X locally, then serialize via tree on package (trying
   * twice at each level), then serialize via tree over packages.
   */
  void doublePolicy(const Item& item) {
    Item newitem = { item.val, item.retries + 1 };
    if ((item.retries & 1) == 1) {
      queues.getLocal()->push(newitem);
      return;
    } 
    
    unsigned tid = LL::getTID();
    unsigned package = LL::getPackageForSelf(tid);
    unsigned leader = LL::getLeaderForPackage(package);
    if (tid != leader) {
      unsigned next = leader + (tid - leader) / 2;
      queues.getRemote(next)->push(newitem);
    } else {
      queues.getRemote(LL::getLeaderForPackage(package / 2))->push(newitem);
    }
  }

  /**
   * Policy: retry work 2X locally, then serialize via tree on package but
   * try at most 3 levels, then serialize via tree over packages.
   */
  void boundedPolicy(const Item& item) {
    Item newitem = { item.val, item.retries + 1 };
    if (item.retries < 2) {
      queues.getLocal()->push(newitem);
      return;
    } 
    
    unsigned tid = LL::getTID();
    unsigned package = LL::getPackageForSelf(tid);
    unsigned leader = LL::getLeaderForPackage(package);
    if (item.retries < 5 && tid != leader) {
      unsigned next = leader + (tid - leader) / 2;
      queues.getRemote(next)->push(newitem);
    } else {
      queues.getRemote(LL::getLeaderForPackage(package / 2))->push(newitem);
    }
  }

  /**
   * Retry locally only.
   */
  void eagerPolicy(const Item& item) {
    Item newitem = { item.val, item.retries + 1 };
    queues.getLocal()->push(newitem);
  }

public:
  value_type& value(Item& item) const { return item.val; }
  value_type& value(value_type& val) const { return val; }

  void push(const value_type& val) {
    Item item = { val, 1 };
    queues.getLocal()->push(item);
  }

  void push(const Item& item) {
    doublePolicy(item);
  }

  AbortedList* getQueue() { return queues.getLocal(); }
};

template<typename FunctionTy>
class BreakHandler {
  LL::CacheLineStorage<bool> broke;

public:
  BreakHandler(): broke(false) { }

  void updateBreak() { 
    if (ForEachTraits<FunctionTy>::NeedsBreak)
      broke.data = true; 
  }

  bool checkBreak() {
    if (ForEachTraits<FunctionTy>::NeedsBreak)
      return broke.data;
    else
      return false;
  }
};

template<class WorkListTy, class T, class FunctionTy>
class ForEachWork {
protected:
  typedef T value_type;
  typedef typename WorkListTy::template retype<value_type>::type WLTy;

  struct ThreadLocalData {
    FunctionTy function;
    UserContextAccess<value_type> facing;
    SimpleRuntimeContext cnx;
    LoopStatistics<ForEachTraits<FunctionTy>::NeedsStats> stat;
    ThreadLocalData(const FunctionTy& fn, const char* ln): function(fn), stat(ln) {}
  };

  // NB: Place dynamically growing wl after fixed-size PerThreadStorage
  // members to give higher likelihood of reclaiming PerThreadStorage
  AbortHandler<value_type> aborted; 
  BreakHandler<FunctionTy> breakHandler;
  TerminationDetection& term;

  WLTy wl;
  FunctionTy& origFunction;
  const char* loopname;

  inline void commitIteration(ThreadLocalData& tld) {
    if (ForEachTraits<FunctionTy>::NeedsPush) {
      auto ii = tld.facing.getPushBuffer().begin();
      auto ee = tld.facing.getPushBuffer().end();
      if (ii != ee) {
	wl.push(ii, ee);
	tld.facing.resetPushBuffer();
      }
    }
    if (ForEachTraits<FunctionTy>::NeedsPIA)
      tld.facing.resetAlloc();
    if (ForEachTraits<FunctionTy>::NeedsAborts)
      tld.cnx.commit_iteration();
  }

  template<typename Item>
  GALOIS_ATTRIBUTE_NOINLINE
  void abortIteration(const Item& item, ThreadLocalData& tld) {
    assert(ForEachTraits<FunctionTy>::NeedsAborts);
    tld.cnx.cancel_iteration();
    tld.stat.inc_conflicts(); //Class specialization handles opt
    aborted.push(item);
    //clear push buffer
    if (ForEachTraits<FunctionTy>::NeedsPush)
      tld.facing.resetPushBuffer();
    //reset allocator
    if (ForEachTraits<FunctionTy>::NeedsPIA)
      tld.facing.resetAlloc();
  }

#ifdef GALOIS_USE_HTM
# ifndef GALOIS_USE_LONGJMP
#  error "HTM must be used with GALOIS_USE_LONGJMP"
# endif

  inline void runFunctionWithHTMandBreak(value_type& val, ThreadLocalData& tld) {
    int result;
#pragma tm_atomic
    {
      if ((result = setjmp(hackjmp)) == 0) {
        tld.function(val, tld.facing.data());
      } else { clearConflictLock(); }
      clearReleasable();
    }
    switch (result) {
    case 0:
      break;
    case BREAK:
      handleBreak(tld);
      throw result;
    default:
      GALOIS_DIE("unknown conflict type");
    }
  }

  inline void runFunction(value_type& val, ThreadLocalData& tld) {
    int flag = 0;
    if (ForEachTraits<FunctionTy>::NeedsBreak) {
      runFunctionWithHTMandBreak(val, tld);
    } else {
#pragma tm_atomic
      {
        tld.function(val, tld.facing.data());
      }
    }
  }
#else
  inline void runFunction(value_type& val, ThreadLocalData& tld) {
    tld.function(val, tld.facing.data());
  }
#endif

  inline void doProcess(value_type& val, ThreadLocalData& tld) {
    tld.stat.inc_iterations();
    if (ForEachTraits<FunctionTy>::NeedsAborts)
      tld.cnx.start_iteration();

    runFunction(val, tld);
    clearReleasable();
    commitIteration(tld);
  }

  GALOIS_ATTRIBUTE_NOINLINE
  void handleBreak(ThreadLocalData& tld) {
    commitIteration(tld);
    breakHandler.updateBreak();
  }

  bool runQueueSimple(ThreadLocalData& tld) {
    bool workHappened = false;
    Galois::optional<value_type> p = wl.pop();
    if (p)
      workHappened = true;
    while (p) {
      doProcess(*p, tld);
      p = wl.pop();
    }
    return workHappened;
  }

  template<bool limit, typename WL>
  bool runQueue(ThreadLocalData& tld, WL& lwl) {
    bool workHappened = false;
    Galois::optional<typename WL::value_type> p = lwl.pop();
    unsigned num = 0;
    int result = 0;
    if (p)
      workHappened = true;
#ifdef GALOIS_USE_LONGJMP
    if ((result = setjmp(hackjmp)) == 0) {
#else
    try {
#endif
      while (p) {
	doProcess(aborted.value(*p), tld);
	if (limit) {
	  ++num;
	  if (num == limit)
	    break;
	}
	p = lwl.pop();
      }
#ifdef GALOIS_USE_LONGJMP
    } else { 
      clearReleasable();
      clearConflictLock(); 
    }
#else
    } catch (ConflictFlag const& flag) {
      clearReleasable();
      clearConflictLock();
      result = flag;
    }
#endif
    switch (result) {
    case 0:
      break;
    case CONFLICT:
      abortIteration(*p, tld);
      break;
    case BREAK:
      handleBreak(tld);
      throw result;
    default:
      GALOIS_DIE("unknown conflict type");
    }
    return workHappened;
  }

  GALOIS_ATTRIBUTE_NOINLINE
  bool handleAborts(ThreadLocalData& tld) {
    return runQueue<0>(tld, *aborted.getQueue());
  }

  void fastPushBack(typename UserContextAccess<value_type>::PushBufferTy& x) {
    wl.push(x.begin(), x.end());
    x.clear();
  }

  template<bool couldAbort, bool isLeader>
  void go() {
    // Thread-local data goes on the local stack to be NUMA friendly
    ThreadLocalData tld(origFunction, loopname);
    if (couldAbort)
      setThreadContext(&tld.cnx);
    if (ForEachTraits<FunctionTy>::NeedsPush && !couldAbort)
      tld.facing.setFastPushBack(
          std::bind(&ForEachWork::fastPushBack, std::ref(*this), std::placeholders::_1));
    bool didWork;
    try {
      do {
        didWork = false;
        // Run some iterations
        if (couldAbort || ForEachTraits<FunctionTy>::NeedsBreak) {
          if (isLeader)
            didWork = runQueue<32>(tld, wl);
          else
            didWork = runQueue<ForEachTraits<FunctionTy>::NeedsBreak ? 32 : 0>(tld, wl);
          // Check for abort
          if (couldAbort)
            didWork |= handleAborts(tld);
        } else { // No try/catch
          didWork = runQueueSimple(tld);
        }
        // Update node color and prop token
        term.localTermination(didWork);
      } while (!term.globalTermination());
    } catch (int flag) { 
    }

    if (couldAbort)
      setThreadContext(0);
  }

public:
  ForEachWork(FunctionTy& f, const char* l): term(getSystemTermination()), origFunction(f), loopname(l) { }
  
  template<typename W>
  ForEachWork(W& w, FunctionTy& f, const char* l): term(getSystemTermination()), wl(w), origFunction(f), loopname(l) { }

  template<typename RangeTy>
  void AddInitialWork(const RangeTy& range) {
    wl.push_initial(range);
  }

  void initThread(void) {
    term.initializeThread();
  }

  void operator()() {
    bool isLeader = LL::isPackageLeaderForSelf(LL::getTID());
    bool couldAbort = ForEachTraits<FunctionTy>::NeedsAborts && activeThreads > 1;
#ifdef GALOIS_USE_HTM
    couldAbort = false;
#endif
    if (couldAbort && isLeader)
      go<true, true>();
    else if (couldAbort && !isLeader)
      go<true, false>();
    else if (!couldAbort && isLeader)
      go<false, true>();
    else
      go<false, false>();
  }
};

template<typename WLTy, typename RangeTy, typename FunctionTy>
void for_each_impl(const RangeTy& range, FunctionTy f, const char* loopname) {
  if (inGaloisForEach)
    GALOIS_DIE("Nested for_each not supported");

  StatTimer LoopTimer("LoopTime", loopname);
  if (ForEachTraits<FunctionTy>::NeedsStats)
    LoopTimer.start();

  inGaloisForEach = true;

  typedef typename RangeTy::value_type T;
  typedef ForEachWork<WLTy, T, FunctionTy> WorkTy;

  // NB: Initialize barrier before creating WorkTy to increase
  // PerThreadStorage reclaimation likelihood
  Barrier& barrier = getSystemBarrier();

  WorkTy W(f, loopname);
  RunCommand w[5] = {
    std::bind(&WorkTy::initThread, std::ref(W)),
    std::bind(&WorkTy::template AddInitialWork<RangeTy>, std::ref(W), range), 
    std::ref(barrier),
    std::ref(W),
    std::ref(barrier)
  };
  getSystemThreadPool().run(&w[0], &w[5], activeThreads);
  if (ForEachTraits<FunctionTy>::NeedsStats)  
    LoopTimer.stop();
  inGaloisForEach = false;
}

template<typename FunctionTy>
struct WOnEach {
  FunctionTy& origFunction;
  WOnEach(FunctionTy& f): origFunction(f) { }
  void operator()(void) {
    FunctionTy fn(origFunction);
    fn(LL::getTID(), activeThreads);   
  }
};

template<typename FunctionTy>
void on_each_impl(FunctionTy fn, const char* loopname = 0) {
  if (inGaloisForEach)
    GALOIS_DIE("Nested for_each not supported");

  inGaloisForEach = true;
  RunCommand w[2] = {WOnEach<FunctionTy>(fn),
		     std::ref(getSystemBarrier())};
  getSystemThreadPool().run(&w[0], &w[2], activeThreads);
  inGaloisForEach = false;
}

//! on each executor with simple barrier.
template<typename FunctionTy>
void on_each_simple_impl(FunctionTy fn, const char* loopname = 0) {
  if (inGaloisForEach)
    GALOIS_DIE("Nested for_each not supported");

  inGaloisForEach = true;
  Barrier* b = createSimpleBarrier();
  b->reinit(activeThreads);
  RunCommand w[2] = {WOnEach<FunctionTy>(fn),
		     std::ref(*b)};
  getSystemThreadPool().run(&w[0], &w[2], activeThreads);
  delete b;
  inGaloisForEach = false;
}

} // end namespace anonymous

void preAlloc_impl(int num);

} // end namespace Runtime
} // end namespace Galois

#endif

