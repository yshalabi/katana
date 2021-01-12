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

#include "katana/analytics/Utils.h"

#include <katana/Random.h>

uint32_t
katana::analytics::SourcePicker::PickNext() {
  uint32_t source;
  do {
    source = RandomUniformInt(graph.size());
  } while (
      std::distance(graph.edges(source).begin(), graph.edges(source).end()));
  return source;
}

bool
katana::analytics::IsApproximateDegreeDistributionPowerLaw(
    const PropertyFileGraph& graph) {
  if (graph.num_nodes() < 10) {
    return false;
  }
  uint32_t averageDegree = graph.num_edges() / graph.num_nodes();
  if (averageDegree < 10) {
    return false;
  }
  SourcePicker sp(graph);
  uint32_t num_samples = 1000;
  if (num_samples > graph.num_nodes()) {
    num_samples = graph.num_nodes();
  }
  uint32_t sample_total = 0;
  std::vector<uint32_t> samples(num_samples);
  for (uint32_t trial = 0; trial < num_samples; trial++) {
    auto node = sp.PickNext();
    samples[trial] =
        std::distance(graph.edges(node).begin(), graph.edges(node).end());
    sample_total += samples[trial];
  }
  std::sort(samples.begin(), samples.end());
  double sample_average = static_cast<double>(sample_total) / num_samples;
  double sample_median = samples[num_samples / 2];
  return sample_average / 1.3 > sample_median;
}