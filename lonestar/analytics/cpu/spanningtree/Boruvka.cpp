/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
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
 */

#include <algorithm>
#include <atomic>
#include <iostream>
#include <utility>

#include "Lonestar/BoilerPlate.h"
#include "katana/Bag.h"
#include "katana/Galois.h"
#include "katana/LCGraph.h"
#include "katana/ParallelSTL.h"
#include "katana/Profile.h"
#include "katana/Reduction.h"
#include "katana/Timer.h"
#include "katana/UnionFind.h"
#include "llvm/Support/CommandLine.h"

namespace cll = llvm::cl;

static const char* name = "Boruvka's Minimum Spanning Tree Algorithm";
static const char* desc = "Computes the minimum spanning forest of a graph";
static const char* url = "mst";

enum Algo { parallel, exp_parallel };

static cll::opt<std::string> inputFilename(
    cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<Algo> algo(
    "algo", cll::desc("Choose an algorithm (default value parallel):"),
    cll::values(clEnumVal(parallel, "Parallel")), cll::init(parallel));

typedef int EdgeData;

struct Node : public katana::UnionFindNode<Node> {
  std::atomic<EdgeData*> lightest;
  Node() : katana::UnionFindNode<Node>(const_cast<Node*>(this)) {}
};

typedef katana::LC_CSR_Graph<Node, EdgeData>::with_numa_alloc<
    true>::type ::with_no_lockable<true>::type Graph;

typedef Graph::GraphNode GNode;

std::ostream&
operator<<(std::ostream& os, const Node& n) {
  os << "[id: " << &n << ", c: " << n.find() << "]";
  return os;
}

struct Edge {
  GNode src;
  GNode dst;
  const EdgeData* weight;
  Edge(const GNode& s, const GNode& d, const EdgeData* w)
      : src(s), dst(d), weight(w) {}
};

/**
 * Boruvka's algorithm. Implemented bulk-synchronously in order to avoid the
 * need to merge edge lists.
 */
template <bool useExp>
struct ParallelAlgo {
  struct WorkItem {
    Edge edge;
    int cur;
    WorkItem(const GNode& s, const GNode& d, const EdgeData* w, int c)
        : edge(s, d, w), cur(c) {}
  };

  typedef katana::InsertBag<WorkItem> WL;

  Graph graph;

  WL wls[3];
  WL* current;
  WL* next;
  WL* pending;
  EdgeData limit;
  katana::InsertBag<Edge> mst;
  EdgeData inf;
  EdgeData heaviest;

  /**
   * Find lightest edge between components leaving a node and add it to the
   * worklist.
   */
  template <bool useLimit, typename Context, typename Pending>
  static void findLightest(
      ParallelAlgo* self, const GNode& src, int cur, Context& ctx,
      Pending& pending) {
    Node& sdata = self->graph.getData(src, katana::MethodFlag::UNPROTECTED);
    Graph::edge_iterator ii =
        self->graph.edge_begin(src, katana::MethodFlag::UNPROTECTED);
    Graph::edge_iterator ei =
        self->graph.edge_end(src, katana::MethodFlag::UNPROTECTED);

    std::advance(ii, cur);

    for (; ii != ei; ++ii, ++cur) {
      GNode dst = self->graph.getEdgeDst(ii);
      Node& ddata = self->graph.getData(dst, katana::MethodFlag::UNPROTECTED);
      EdgeData& weight = self->graph.getEdgeData(ii);
      if (useLimit && weight > self->limit) {
        pending.push(WorkItem(src, dst, &weight, cur));
        return;
      }
      Node* rep;
      if ((rep = sdata.findAndCompress()) != ddata.findAndCompress()) {
        // const EdgeData& weight = self->graph.getEdgeData(ii);
        EdgeData* old;
        ctx.push(WorkItem(src, dst, &weight, cur));
        while (weight < *(old = rep->lightest)) {
          if (rep->lightest.compare_exchange_strong(old, &weight))
            break;
        }
        return;
      }
    }
  }

  /**
   * Merge step specialized for first round of the algorithm.
   */
  struct Initialize {
    ParallelAlgo* self;

    Initialize(ParallelAlgo* s) : self(s) {}

    void operator()(const GNode& src) const {
      (*this)(src, *self->next, *self->pending);
    }

    template <typename Context>
    void operator()(const GNode& src, Context& ctx) const {
      (*this)(src, ctx, *self->pending);
    }

    template <typename Context, typename Pending>
    void operator()(const GNode& src, Context& ctx, Pending& pending) const {
      Node& sdata = self->graph.getData(src, katana::MethodFlag::UNPROTECTED);
      sdata.lightest = &self->inf;
      findLightest<false>(self, src, 0, ctx, pending);
    }
  };

  struct Merge {
    ParallelAlgo* self;

    Merge(ParallelAlgo* s) : self(s) {}

    void operator()(const WorkItem& item) const {
      (*this)(item, *self->next, *self->pending);
    }

    template <typename Context>
    void operator()(const WorkItem& item, Context& ctx) const {
      (*this)(item, ctx, *self->pending);
    }

    template <typename Context, typename Pending>
    void operator()(const WorkItem& item, Context&, Pending&) const {
      GNode src = item.edge.src;
      Node& sdata = self->graph.getData(src, katana::MethodFlag::UNPROTECTED);
      Node* rep = sdata.findAndCompress();
      int cur = item.cur;

      if (rep->lightest == item.edge.weight) {
        GNode dst = item.edge.dst;
        Node& ddata = self->graph.getData(dst, katana::MethodFlag::UNPROTECTED);
        if ((rep = sdata.merge(&ddata))) {
          rep->lightest = &self->inf;
          self->mst.push(Edge(src, dst, item.edge.weight));
        }
        ++cur;
      }
    }
  };

  struct Find {
    ParallelAlgo* self;

    Find(ParallelAlgo* s) : self(s) {}

    void operator()(const WorkItem& item) const {
      (*this)(item, *self->next, *self->pending);
    }

    template <typename Context>
    void operator()(const WorkItem& item, Context& ctx) const {
      (*this)(item, ctx, *self->pending);
    }

    template <typename Context, typename Pending>
    void operator()(
        const WorkItem& item, Context& ctx, Pending& pending) const {
      findLightest<true>(self, item.edge.src, item.cur, ctx, pending);
    }
  };

  void init() {
    current = &wls[0];
    next = &wls[1];
    pending = &wls[2];

    EdgeData delta = std::max(heaviest / 5, 1);
    limit = delta;
  }

  void process() {
    constexpr unsigned CHUNK_SIZE = 16;

    size_t rounds = 0;

    init();

    katana::do_all(
        katana::iterate(graph), Initialize(this),
        katana::chunk_size<CHUNK_SIZE>(), katana::steal(),
        katana::loopname("Initialize"));

    while (true) {
      while (true) {
        rounds += 1;

        std::swap(current, next);
        katana::do_all(
            katana::iterate(*current), Merge(this), katana::steal(),
            katana::chunk_size<CHUNK_SIZE>(), katana::loopname("Merge"));
        katana::do_all(
            katana::iterate(*current), Find(this), katana::steal(),
            katana::chunk_size<CHUNK_SIZE>(), katana::loopname("Find"));
        current->clear();

        if (next->empty())
          break;
      }

      if (pending->empty())
        break;

      std::swap(next, pending);

      limit *= 2;
    }

    katana::ReportStatSingle("Boruvka", "rounds", rounds);
  }

  void processExp() { KATANA_DIE("not supported"); }

  void operator()() {
    if (useExp) {
      processExp();
    } else {
      process();
    }
  }

  bool checkAcyclic(void) {
    katana::GAccumulator<unsigned> roots;

    katana::do_all(katana::iterate(graph), [&roots, this](const GNode& n) {
      const auto& data = graph.getData(n, katana::MethodFlag::UNPROTECTED);
      if (data.isRep())
        roots += 1;
    });

    unsigned numRoots = roots.reduce();
    unsigned numEdges = std::distance(mst.begin(), mst.end());

    if (graph.size() - numRoots != numEdges) {
      std::cerr << "Generated graph is not a forest. "
                << "Expected " << graph.size() - numRoots << " edges but "
                << "found " << numEdges << "\n";
      return false;
    }

    std::cout << "Num trees: " << numRoots << "\n";
    std::cout << "Tree edges: " << numEdges << "\n";
    return true;
  }

  EdgeData sortEdges() {
    katana::GReduceMax<EdgeData> heavy;

    katana::do_all(katana::iterate(graph), [&heavy, this](const GNode& src) {
      //! [sortEdgeByEdgeData]
      graph.sortEdgesByEdgeData(
          src, std::less<EdgeData>(), katana::MethodFlag::UNPROTECTED);
      //! [sortEdgeByEdgeData]

      Graph::edge_iterator ii =
          graph.edge_begin(src, katana::MethodFlag::UNPROTECTED);
      Graph::edge_iterator ei =
          graph.edge_end(src, katana::MethodFlag::UNPROTECTED);
      ptrdiff_t dist = std::distance(ii, ei);
      if (dist == 0)
        return;
      std::advance(ii, dist - 1);
      heavy.update(graph.getEdgeData(ii));
    });

    return heavy.reduce();
  }

  bool verify() {
    auto is_bad_graph = [this](const GNode& n) {
      Node& me = graph.getData(n);
      for (auto ii : graph.edges(n)) {
        GNode dst = graph.getEdgeDst(ii);
        Node& data = graph.getData(dst);
        if (me.findAndCompress() != data.findAndCompress()) {
          std::cerr << "not in same component: " << me << " and " << data
                    << "\n";
          return true;
        }
      }
      return false;
    };

    auto is_bad_mst = [this](const Edge& e) {
      return graph.getData(e.src).findAndCompress() !=
             graph.getData(e.dst).findAndCompress();
    };

    if (katana::ParallelSTL::find_if(
            graph.begin(), graph.end(), is_bad_graph) == graph.end()) {
      if (katana::ParallelSTL::find_if(mst.begin(), mst.end(), is_bad_mst) ==
          mst.end()) {
        return checkAcyclic();
      }
    }
    return false;
  }

  void initializeGraph() {
    katana::FileGraph origGraph;
    katana::FileGraph symGraph;

    origGraph.fromFileInterleaved<EdgeData>(inputFilename);
    if (!symmetricGraph)
      katana::makeSymmetric<EdgeData>(origGraph, symGraph);
    else
      std::swap(symGraph, origGraph);

    katana::readGraph(graph, symGraph);

    katana::StatTimer Tsort("InitializeSortTime");
    Tsort.start();
    heaviest = sortEdges();
    if (heaviest == std::numeric_limits<EdgeData>::max() ||
        heaviest == std::numeric_limits<EdgeData>::min()) {
      KATANA_DIE("Edge weights of graph out of range");
    }
    inf = heaviest + 1;

    Tsort.stop();

    std::cout << "Nodes: " << graph.size() << " edges: " << graph.sizeEdges()
              << " heaviest edge: " << heaviest << "\n";
  }
};

template <typename Algo>
void
run() {
  Algo algo;

  katana::StatTimer Tinitial("InitializeTime");
  Tinitial.start();
  algo.initializeGraph();
  Tinitial.stop();

  katana::Prealloc(8, 16 * (algo.graph.size() + algo.graph.sizeEdges()));
  katana::ReportPageAllocGuard page_alloc;

  katana::StatTimer execTime("Boruvka");
  execTime.start();
  katana::profileVtune([&](void) { algo(); }, "boruvka");
  execTime.stop();

  page_alloc.Report();

  auto get_weight = [](const Edge& e) { return *e.weight; };

  auto w = katana::ParallelSTL::map_reduce(
      algo.mst.begin(), algo.mst.end(), get_weight, std::plus<size_t>(), 0UL);

  std::cout << "MST weight: " << w << "\n";

  if (!skipVerify && !algo.verify()) {
    KATANA_DIE("verification failed");
  }
}

int
main(int argc, char** argv) {
  std::unique_ptr<katana::SharedMemSys> G =
      LonestarStart(argc, argv, name, desc, url, &inputFilename);

  katana::StatTimer totalTime("TimerTotal");
  totalTime.start();

  switch (algo) {
  case parallel:
    run<ParallelAlgo<false>>();
    break;
  case exp_parallel:
    run<ParallelAlgo<true>>();
    break;
  default:
    std::cerr << "Unknown algo: " << algo << "\n";
  }

  totalTime.stop();

  return 0;
}
