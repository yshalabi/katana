#ifndef MINER_H_
#define MINER_H_
#include "miner.h"
#include "egonet.h"
#include "edgelist.h"
#include "embedding_list.h"

class VertexMiner : public Miner {
public:
	VertexMiner(Graph *g, unsigned size = 3, int np = 1, bool use_dag = true, unsigned c = 0) {
		graph = g;
		max_size = size;
		npatterns = np;
		is_dag = use_dag;
		core = c;
		if (npatterns == 1) total_num.reset();
		else {
			accumulators.resize(npatterns);
			for (int i = 0; i < npatterns; i++) accumulators[i].reset();
			std::cout << max_size << "-motif has " << npatterns << " patterns in total\n";
		}
		degree_counting();
		for (int i = 0; i < numThreads; i++) {
			#ifdef USE_EGONET
			egonets.getLocal(i)->allocate(core, size);
			#endif
			emb_lists.getLocal(i)->allocate(core, size);
		}
		edge_list.init(*g, is_dag);
		// connected k=3 motifs
		total_3_tris = 0;
		total_3_path = 0;
		// connected k=4 motifs
		total_4_clique = 0;
		total_4_diamond = 0;
		total_4_tailed_tris = 0;
		total_4_cycle = 0;
		total_3_star = 0;
		total_4_path = 0;
	}
	virtual ~VertexMiner() {}

	// Pangolin APIs
	// toExtend
	virtual bool toExtend(unsigned n, const BaseEmbedding &emb, unsigned pos) {
		return true;
	}
	// toAdd (only add non-automorphisms)
	virtual bool toAdd(unsigned n, const BaseEmbedding &emb, VertexId dst, unsigned pos) {
		return true;
	}
	virtual bool toExtend(unsigned n, const VertexEmbedding &emb, unsigned pos) {
		return true;
	}
	virtual bool toAdd(unsigned n, const VertexEmbedding &emb, VertexId dst, unsigned pos) {
		VertexId src = emb.get_vertex(pos);
		return !is_vertexInduced_automorphism<VertexEmbedding>(n, emb, pos, src, dst);
	}
	virtual unsigned getPattern(unsigned n, unsigned i, VertexId dst, const VertexEmbedding &emb, unsigned pos) {
		return 0;
	}
	virtual void reduction(unsigned pid) {
		#ifdef USE_MAP
		accumulators[pid] += 1;
		#else
		total_num += 1;
		#endif
	}
	virtual void print_output() {
	}
	// naive DFS extension for k-cliques
	void dfs_extend_naive(unsigned level, unsigned pos, EmbeddingList &emb_list) {
		unsigned n = level + 1;
		BaseEmbedding emb(n);
		emb_list.get_embedding<BaseEmbedding>(level, pos, emb);
		if (level == max_size-2) {
			//for (unsigned i = 0; i < emb_list.size(level); i++) { //list all edges
				//unsigned vid = emb_list.get_vertex(level, i);
				unsigned vid = emb_list.get_vertex(level, pos);
				auto begin = graph->edge_begin(vid);
				auto end = graph->edge_end(vid);
				for (auto e = begin; e != end; e ++) {
					auto dst = graph->getEdgeDst(e);
					if (toAdd(n, emb, dst, n-1))
						//total_num += 1;
						reduction(0);
				}
			//}
			return;
		}
		//for(unsigned i = 0; i < emb_list.size(level); i ++) {
			//unsigned vid = emb_list.get_vertex(level, i);
			unsigned vid = emb_list.get_vertex(level, pos);
			auto begin = graph->edge_begin(vid);
			auto end = graph->edge_end(vid);
			emb_list.set_size(level+1, 0);
			for (auto e = begin; e < end; e ++) {
				auto dst = graph->getEdgeDst(e);
				if (toAdd(n, emb, dst, n-1)) {
					unsigned start = emb_list.size(level+1);
					emb_list.set_vid(level+1, start, dst);
					//emb_list.set_idx(level+1, start, i);
					emb_list.set_idx(level+1, start, pos);
					emb_list.set_size(level+1, start+1);
					dfs_extend_naive(level+1, start, emb_list);
				}
			//}
			//dfs_extend_naive(level+1, i, emb_list);
		}
	}
	// each task extends from a vertex, level starts from k-1 and decreases until bottom level
	void dfs_extend(unsigned level, Egonet &egonet, EmbeddingList &emb_list) {
		if (level == 2) {
			for(unsigned local_id = 0; local_id < emb_list.size(level); local_id ++) { //list all edges
				unsigned vid = emb_list.get_vertex(level, local_id);
				unsigned begin = egonet.edge_begin(vid);
				unsigned end = begin + egonet.get_degree(level, vid);
				for (unsigned e = begin; e < end; e ++)
					total_num += 1;
			}
			return;
		}
		// compute the subgraphs induced on the neighbors of each node in current level,
		// and then recurse on such a subgraph
		for(unsigned local_id = 0; local_id < emb_list.size(level); local_id ++) {
			// for each vertex in current level
			// a new induced subgraph G[∆G(u)] is built
			unsigned vid = emb_list.get_vertex(level, local_id);
			emb_list.set_size(level-1, 0);
			unsigned begin = egonet.edge_begin(vid);
			unsigned end = begin + egonet.get_degree(level, vid);
			// extend one vertex v which is a neighbor of u
			for (unsigned edge = begin; edge < end; edge ++) {
				// for each out-neighbor v of node u in G, set its label to level-1
				// if the label was equal to level. We thus have that if a label of a
				// node v is equal to level-1 it means that node v is in the new subgraph
				unsigned dst = egonet.getEdgeDst(edge);
				// relabeling vertices and forming U'.
				if (emb_list.get_label(dst) == level) {
					unsigned pos = emb_list.size(level-1);
					emb_list.set_vertex(level-1, pos, dst);
					emb_list.set_size(level-1, pos+1);
					emb_list.set_label(dst, level-1);
					egonet.set_degree(level-1, dst, 0);//new degrees
				}
			}
			// for each out-neighbor v of u
			// reodering adjacency list and computing new degrees
			unsigned new_size = emb_list.size(level-1);
			if(debug) printf("debug: vid = %d, new_size = %d\n", vid, new_size);
			for (unsigned j = 0; j < new_size; j ++) {
				unsigned v = emb_list.get_vertex(level-1, j);
				begin = v * core;
				end = begin + egonet.get_degree(level, v);
				// move all the out-neighbors of v with label equal to level − 1 
				// in the first part of ∆(v) (by swapping nodes),
				// and compute the out-degree of node v in the new subgraph
				// updating degrees(v). The first degrees(v) nodes in ∆(v) are
				// thus the out-neighbors of v in the new subgraph.
				for (unsigned k = begin; k < end; k ++) {
					unsigned dst = egonet.getEdgeDst(k);
					if (emb_list.get_label(dst) == level-1)
						egonet.inc_degree(level-1, v);
					else {
						egonet.set_adj(k--, egonet.getEdgeDst(--end));
						egonet.set_adj(end, dst);
					}
				}
			}
			dfs_extend(level-1, egonet, emb_list);
			for (unsigned j = 0; j < emb_list.size(level-1); j ++) {//restoring labels
				unsigned v = emb_list.get_vertex(level-1, j);
				emb_list.set_label(v, level);
			}
		}
	}

	void edge_process_naive() {
		//galois::do_all(galois::iterate(edge_list.begin(), edge_list.end()),
		//	[&](const Edge &edge) {
		galois::do_all(galois::iterate((size_t)0, edge_list.size()),
			[&](const size_t& pos) {
				EmbeddingList *emb_list = emb_lists.getLocal();
				emb_list->init(edge_list.get_edge(pos));
				#ifdef USE_MAP
				dfs_extend_naive_motif(1, 0, *emb_list, 0);
				#else
				dfs_extend_naive(1, 0, *emb_list);
				#endif
			},
			galois::chunk_size<CHUNK_SIZE>(), galois::steal(), galois::no_conflicts(),
			galois::loopname("DfsEdgeNaiveSolver")
		);
		#ifdef USE_MAP
		motif_count();
		#endif
	}

	void edge_process_adhoc() {
		std::cout << "Starting Ad-Hoc Motif Solver\n";
		if (max_size > 4) {
			std::cout << "not supported yet\n";
			return;
		}
		galois::do_all(galois::iterate((size_t)0, edge_list.size()),
			[&](const size_t& pos) {
				EmbeddingList *emb_list = emb_lists.getLocal();
				emb_list->init(edge_list.get_edge(pos));
				dfs_from_edge_adhoc(1, 0, *emb_list);
			},
			galois::chunk_size<CHUNK_SIZE>(), galois::steal(), galois::no_conflicts(),
			galois::loopname("DfsAdhocSolver")
		);
		motif_count();
	}

	void dfs_extend_naive_motif(unsigned level, unsigned pos, EmbeddingList &emb_list, unsigned previous_pid) {
		unsigned n = level + 1;
		VertexEmbedding emb(n);
		emb_list.get_embedding<VertexEmbedding>(level, pos, emb);
		if (level == max_size-2) {
			// extending every vertex in the embedding
			for (unsigned element_id = 0; element_id < n; ++ element_id) {
				if(!toExtend(n, emb, element_id)) continue;
				VertexId src = emb.get_vertex(element_id);
				auto begin = graph->edge_begin(src);
				auto end = graph->edge_end(src);
				for (auto e = begin; e != end; e ++) {
					auto dst = graph->getEdgeDst(e);
					if (toAdd(n, emb, dst, element_id))
						reduction(getPattern(n, element_id, dst, emb, previous_pid));
				}
			}
			return;
		}
		// extending every vertex in the embedding
		for (unsigned element_id = 0; element_id < n; ++ element_id) {
			if(!toExtend(n, emb, element_id)) continue;
			VertexId src = emb.get_vertex(element_id);
			auto begin = graph->edge_begin(src);
			auto end = graph->edge_end(src);
			emb_list.set_size(level+1, 0);
			for (auto e = begin; e < end; e ++) {
				auto dst = graph->getEdgeDst(e);
				if (toAdd(n, emb, dst, element_id)) {
					unsigned start = emb_list.size(level+1);
					emb_list.set_vid(level+1, start, dst);
					emb_list.set_idx(level+1, start, pos);
					emb_list.set_size(level+1, start+1);
					//if (n == 2 && max_size == 4)
					unsigned pid = find_motif_pattern_id_dfs(n, element_id, dst, emb, start);
					dfs_extend_naive_motif(level+1, start, emb_list, pid);
				}
			}
		}
	}

	// construct the subgraph induced by edge (u, v)'s neighbors
	void dfs_from_edge_adhoc(unsigned level, unsigned pos, EmbeddingList &emb_list) {
		unsigned n = level + 1;
		VertexEmbedding emb(n);
		emb_list.get_embedding<VertexEmbedding>(level, pos, emb);
		unsigned u = emb.get_vertex(0), v = emb.get_vertex(1);
		//std::cout << "Edge: " << u << " --> " << v << "\n";
		UintList *ids = id_lists.getLocal();
		if (ids->empty()) {
			ids->resize(graph->size());
			std::fill(ids->begin(), ids->end(), 0);
		}

		UintList *T_vu = Tri_vids.getLocal(); // to record the third vertex in each triangle
		UintList *W_u = Wed_vids.getLocal(); //  to record the third vertex in each wedge
		if (W_u->empty()) {
			T_vu->resize(core+1); // hold the vertices that form a triangle with u and v
			W_u->resize(core+1); // hold the vertices that form a wedge with u and v
			std::fill(T_vu->begin(), T_vu->end(), 0);
			std::fill(W_u->begin(), W_u->end(), 0);
		}

		Ulong wedge_count = 0, tri_count = 0;
		mark_neighbors(v, u, *ids);
		unsigned deg_v = std::distance(graph->edge_begin(v), graph->edge_end(v));
		unsigned deg_u = std::distance(graph->edge_begin(u), graph->edge_end(u));
		if (max_size == 3) {
			auto begin = graph->edge_begin(u);
			auto end = graph->edge_end(u);
			for (auto e = begin; e < end; e ++) {
				auto w = graph->getEdgeDst(e);
				if (w == v) continue;
				if ((*ids)[w] == 1) tri_count++;
			}
			accumulators[0] += tri_count;
			accumulators[1] += deg_v - tri_count - 1 + deg_u - tri_count - 1;
		} else if (max_size == 4) {
			triangles_and_wedges(v, u, *T_vu, tri_count, *W_u, wedge_count, *ids);
			Ulong clique4_count = 0, cycle4_count = 0;
			solve_graphlet_equations(deg_v, deg_u, tri_count, wedge_count);
			cycle(wedge_count, *W_u, cycle4_count, *ids);
			clique(tri_count, *T_vu, clique4_count, *ids);
			accumulators[5] += clique4_count;
			accumulators[2] += cycle4_count;
		} else {
		}
		reset_perfect_hash(v, *ids);
	}

	void dfs_extend_motif(unsigned level, unsigned pos, EmbeddingList &emb_list, unsigned previous_pid) {
		unsigned n = level + 1;
		VertexEmbedding emb(n);
		emb_list.get_embedding<VertexEmbedding>(level, pos, emb);
		UintList *ids = id_lists.getLocal();
		if (ids->empty()) {
			ids->resize(graph->size());
			std::fill(ids->begin(), ids->end(), 0);
		}

			/*
		if (level == max_size-2) {
			if (max_size == 4) {
				Ulong clique4_count = 0, cycle4_count = 0;
				cycle(wedge_count, *W_u, cycle4_count, *ids);
				clique(tri_count, *T_vu, clique4_count, *ids);
				accumulators[5] += clique4_count;
				accumulators[2] += cycle4_count;
				reset_perfect_hash(v, *ids);
				return;
			}
			// extending every vertex in the embedding
			for (unsigned element_id = 0; element_id < n; ++ element_id) {
				if(!toExtend(n, emb, element_id)) continue;
				VertexId src = emb.get_vertex(element_id);
				auto begin = graph->edge_begin(src);
				auto end = graph->edge_end(src);
				for (auto e = begin; e != end; e ++) {
					auto dst = graph->getEdgeDst(e);
					if (toAdd(n, emb, dst, element_id))
						reduction(getPattern(n, element_id, dst, emb, previous_pid));
				}
			}
			return;
		}
			*/

		unsigned v0 = emb.get_vertex(0), v1 = emb.get_vertex(1);
		UintList *T_vu = Tri_vids.getLocal(); // to record the third vertex in each triangle
		UintList *W_u = Wed_vids.getLocal(); //  to record the third vertex in each wedge
		if (max_size == 4) {
			if (W_u->empty()) {
				T_vu->resize(core+1); // hold the vertices that form a triangle with u and v
				W_u->resize(core+1); // hold the vertices that form a wedge with u and v
				std::fill(T_vu->begin(), T_vu->end(), 0);
				std::fill(W_u->begin(), W_u->end(), 0);
			}
		}
		Ulong wedge_count = 0, tri_count = 0;
		mark_neighbors(v0, v1, *ids);
		triangles_and_wedges(v0, v1, *T_vu, tri_count, *W_u, wedge_count, *ids);
/*
		// extending every vertex in the embedding
		for (unsigned element_id = 0; element_id < n; ++ element_id) {
			//if (!toExtend(n, emb, element_id)) continue;
			if (element_id != n-1) continue;
			VertexId src = emb.get_vertex(element_id);
			auto begin = graph->edge_begin(src);
			auto end = graph->edge_end(src);
			emb_list.set_size(level+1, 2*core);
			for (auto e = begin; e < end; e ++) {
				auto dst = graph->getEdgeDst(e);
				if (toAdd(n, emb, dst, element_id)) {
					unsigned pid = find_motif_pattern_id_dfs(n, element_id, dst, emb, start);
					if (n == 2 && max_size == 4)
					unsigned start = emb_list.size(level+1);
					emb_list.set_vid(level+1, start, dst);
					emb_list.set_idx(level+1, start, pos);
					//emb_list.set_size(level+1, start+1);
					dfs_extend_naive_motif(level+1, start, emb_list, pid);
				}
			}
		}
		*/
		if (n == 2 && max_size == 4) {
			unsigned deg_v1 = std::distance(graph->edge_begin(v1), graph->edge_end(v1));
			unsigned deg_v0 = std::distance(graph->edge_begin(v0), graph->edge_end(v0));
			solve_graphlet_equations(deg_v1, deg_v0, tri_count, wedge_count);
		}
	}

	// construct the subgraph induced by vertex u's neighbors
	void build_egonet_from_vertex(const GNode &u, Egonet &egonet, EmbeddingList &emb_list, UintList &ids, UintList &old_id) {
		if (ids.empty()) {
			ids.resize(graph->size());
			//old_id.resize(core);
			for (unsigned i = 0; i < graph->size(); i ++) ids[i] = (unsigned)-1;
		}
		//UintList ids(graph->size(), (unsigned)-1);
		//ids.resize(graph->size());
		//for (unsigned i = 0; i < ids.size(); i ++) ids[i] = (unsigned)-1;
		unsigned level = max_size-1;
		if(debug) printf("\n\n=======================\ninit: u = %d, level = %d\n", u, level);
		for (unsigned i = 0; i < emb_list.size(level); i ++) emb_list.set_label(i, 0);
		unsigned new_size = 0;
		for (auto e : graph->edges(u)) {
			auto v = graph->getEdgeDst(e);
			ids[v] = new_size;
			//old_id[new_size] = v;
			emb_list.set_label(new_size, level);
			emb_list.set_vertex(level, new_size, new_size);
			egonet.set_degree(level, new_size, 0);//new degrees
			if(debug) printf("init: v[%d] = %d\n", new_size, v);
			new_size ++;
		}
		if(debug) printf("init: num_neighbors = %d\n", new_size);
		emb_list.set_size(level, new_size); // number of neighbors of u. Since u is in level k, u's neighbors are in level k-1
		//reodering adjacency list and computing new degrees
		unsigned i = 0;
		for (auto e0 : graph->edges(u)) {
			auto v = graph->getEdgeDst(e0);
		//for (unsigned i = 0; i < emb_list.size(level); i ++) {
		//	unsigned v = old_id[i];
			// intersection of two neighbor lists
			for (auto e : graph->edges(v)) {
				auto dst = graph->getEdgeDst(e); // dst is the neighbor's neighbor
				unsigned new_id = ids[dst];
				if (new_id != (unsigned)-1) { // if dst is also a neighbor of u
					//if (max_size == 3) total_num += 1; //listing here!!!
					//else {
						unsigned degree = egonet.get_degree(level, i);
						egonet.set_adj(core * i + degree, new_id); // relabel
						egonet.set_degree(level, i, degree+1);
					//}
				}
			}
			if(debug) printf("vertex %d, number of common neighbors: %d\n", v, egonet.get_degree(level, i));
			i ++;
		}
		for (auto e : graph->edges(u)) {
			auto v = graph->getEdgeDst(e);
			ids[v] = (unsigned)-1;
		}
	}

	// construct the subgraph induced by vertex u's neighbors
	void build_egonet_from_edge(const Edge &edge, Egonet &egonet, EmbeddingList &emb_list, UintList &ids, UintList &old_id) {
		unsigned u = edge.src, v = edge.dst;
		if (old_id.empty()) {
			ids.resize(graph->size());
			old_id.resize(core);
			for (unsigned i = 0; i < graph->size(); i ++) ids[i] = (unsigned)-1;
		}
		unsigned level = max_size-1;
		if(debug) printf("\n\n=======================\ninit: u = %d, v = %d, level = %d\n", u, v, level);
		for (unsigned i = 0; i < emb_list.size(level); i ++) emb_list.set_label(i, 0);
		for (auto e : graph->edges(v)) {
			auto dst = graph->getEdgeDst(e);
			ids[dst] = (unsigned)-2;
		}
		unsigned new_size = 0;
		for (auto e : graph->edges(u)) {
			auto dst = graph->getEdgeDst(e);
			if (ids[dst] == (unsigned)-2) {
				if (max_size == 3) total_num += 1;
				else {
					ids[dst] = new_size;
					old_id[new_size] = dst;
					emb_list.set_label(new_size, max_size-2);
					emb_list.set_vertex(max_size-2, new_size, new_size);
					egonet.set_degree(max_size-2, new_size, 0);//new degrees
				}
				if(debug) printf("init: v[%d] = %d\n", new_size, dst);
				new_size ++;
			}
		}
		if (max_size > 3) {
			emb_list.set_size(max_size-2, new_size); // number of neighbors of u. Since u is in level k, u's neighbors are in level k-1
			if(debug) printf("init: num_neighbors = %d\n", new_size);
			for (unsigned i = 0; i < emb_list.size(max_size-2); i ++) {
				unsigned x = old_id[i];
				// intersection of two neighbor lists
				for (auto e : graph->edges(x)) {
					auto dst = graph->getEdgeDst(e); // dst is the neighbor's neighbor
					unsigned new_id = ids[dst];
					if (new_id < (unsigned)-2) { // if dst is also a neighbor of u
						unsigned degree = egonet.get_degree(max_size-2, i);
						egonet.set_adj(core * i + degree, new_id); // relabel
						egonet.set_degree(max_size-2, i, degree+1);
					}
				}
				if(debug) printf("vertex %d, number of common neighbors: %d\n", v, egonet.get_degree(level, i));
				//i ++;
			}
		}
		for (auto e : graph->edges(v)) {
			auto dst = graph->getEdgeDst(e);
			ids[dst] = (unsigned)-1;
		}
	}

	// each task extends from a vertex for motif, level starts from k-1 and decreases until bottom level
	void dfs_extend_base(unsigned level, Egonet &egonet, EmbeddingList &emb_list) {
		if (level == 2) {
			for(unsigned i = 0; i < emb_list.size(level); i++) { //list all edges
				unsigned u = emb_list.get_vertex(level, i);
				unsigned label = emb_list.get_label(i);
				auto begin = graph->edge_begin(u);
				auto end = graph->edge_end(u);
				for (auto j = begin; j != end; j ++) {
					if (label == 2) accumulators[0] += 1; // number of k-cliques
				}
			}
			return;
		}
		for(unsigned i = 0; i < emb_list.size(level); i ++) {
			unsigned u = emb_list.get_vertex(level, i);
			emb_list.set_size(level-1, 0);
			auto begin = graph->edge_begin(u);
			auto end = graph->edge_end(u);
			for (auto edge = begin; edge < end; edge ++) {
				auto v = graph->getEdgeDst(edge);
				if (emb_list.get_label(v) == level) {
					unsigned pos = emb_list.size(level-1);
					emb_list.set_vertex(level-1, pos, v);
					emb_list.set_size(level-1, pos+1);
					emb_list.set_label(v, level-1);
				}
			}
			dfs_extend_base(level-1, egonet, emb_list);
			for (unsigned j = 0; j < emb_list.size(level-1); j ++) {//restoring labels
				unsigned v = emb_list.get_vertex(level-1, j);
				emb_list.set_label(v, level);
			}
		}
	}

	void vertex_process() {
		//galois::do_all(galois::iterate((size_t)0, graph->size()),
		galois::for_each(galois::iterate(graph->begin(), graph->end()),
			[&](const size_t &u, auto &ctx) {
		//galois::do_all(galois::iterate(graph->begin(), graph->end()),
		//	[&](const size_t &u) {
				#ifdef USE_EGONET
				Egonet *egonet = egonets.getLocal();
				#else
				Egonet *egonet = NULL;
				#endif
				EmbeddingList *emb_list = emb_lists.getLocal();
				UintList *id_list = id_lists.getLocal();
				UintList *old_id_list = old_id_lists.getLocal();
				build_egonet_from_vertex(u, *egonet, *emb_list, *id_list, *old_id_list);
				//if (max_size > 3)
					dfs_extend(max_size-1, *egonet, *emb_list);
			},
			galois::chunk_size<CHUNK_SIZE>(), galois::steal(), galois::no_conflicts(),
			galois::loopname("DfsVertexSolver")
		);
	}

	void edge_process() {
		//std::cout << "num_edges in edge_list = " << edge_list.size() << "\n\n";
		//galois::do_all(galois::iterate((size_t)0, graph->size()),
		//galois::for_each(galois::iterate(edge_list.begin(), edge_list.end()),
			//[&](const Edge &edge, auto &ctx) {
		galois::do_all(galois::iterate(edge_list.begin(), edge_list.end()),
			[&](const Edge &edge) {
				EmbeddingList *emb_list = emb_lists.getLocal();
				UintList *id_list = id_lists.getLocal();
				UintList *old_id_list = old_id_lists.getLocal();
				Egonet *egonet = egonets.getLocal();
				build_egonet_from_edge(edge, *egonet, *emb_list, *id_list, *old_id_list);
				if (max_size > 3) dfs_extend(max_size-2, *egonet, *emb_list);
			},
			galois::chunk_size<CHUNK_SIZE>(), galois::steal(), galois::no_conflicts(),
			galois::loopname("KclDfsEdgeSolver")
		);
	}

	void motif_count() {
		//std::cout << "[cxh debug] accumulators[0] = " << accumulators[0].reduce() << "\n";
		//std::cout << "[cxh debug] accumulators[3] = " << accumulators[3].reduce() << "\n";
		//std::cout << "[cxh debug] accumulators[1] = " << accumulators[1].reduce() << "\n";
		#ifdef USE_ADHOC
		if (accumulators.size() == 2) {
			if (is_dag) {
				total_3_tris = accumulators[0].reduce();
				total_3_path = accumulators[1].reduce();
			} else {
				total_3_tris = accumulators[0].reduce()/3;
				total_3_path = accumulators[1].reduce()/2;
			}
		} else {
			if (is_dag) {
				total_4_clique = accumulators[5].reduce();
				total_4_diamond = accumulators[4].reduce() - total_4_clique;
				total_4_cycle = accumulators[2].reduce();
				total_4_path = accumulators[0].reduce() - total_4_cycle;
				total_4_tailed_tris = accumulators[3].reduce() - 2*total_4_diamond;
				total_3_star = accumulators[1].reduce() - total_4_tailed_tris;
			} else {
				total_4_clique = accumulators[5].reduce() / 6;
				total_4_diamond = accumulators[4].reduce() - (6*total_4_clique);
				total_4_cycle = accumulators[2].reduce() / 4;
				total_4_path = accumulators[3].reduce() - (4*total_4_cycle);
				total_4_tailed_tris = (accumulators[0].reduce() - (4*total_4_diamond)) / 2;
				total_3_star = (accumulators[1].reduce() - total_4_tailed_tris) / 3;
			}
		}
		#else
		if (accumulators.size() == 2) {
			total_3_tris = accumulators[0].reduce();
			total_3_path = accumulators[1].reduce();
		} else {
			total_4_clique = accumulators[5].reduce();
			total_4_diamond = accumulators[4].reduce();
			total_4_cycle = accumulators[2].reduce();
			total_4_path = accumulators[0].reduce();
			total_4_tailed_tris = accumulators[3].reduce();
			total_3_star = accumulators[1].reduce();
		} 
		#endif
	}

	void printout_motifs() {
		std::cout << std::endl;
		if (accumulators.size() == 2) {
			std::cout << "\ttriangles\t" << total_3_tris << std::endl;
			std::cout << "\t3-paths\t\t" << total_3_path << std::endl;
		} else if (accumulators.size() == 6) {
			std::cout << "\t4-paths --> " << total_4_path << std::endl;
			std::cout << "\t3-stars --> " << total_3_star << std::endl;
			std::cout << "\t4-cycles --> " << total_4_cycle << std::endl;
			std::cout << "\ttailed-triangles --> " << total_4_tailed_tris << std::endl;
			std::cout << "\tdiamonds --> " << total_4_diamond << std::endl;
			std::cout << "\t4-cliques --> " << total_4_clique << std::endl;
		} else {
			std::cout << "Currently not supported!\n";
		}
		//std::cout << std::endl;
	}
	Ulong get_total_count() { return total_num.reduce(); }

protected:
	int npatterns;
	bool is_dag;
	unsigned core;
	UlongAccu total_num;
	std::vector<UlongAccu> accumulators;
	Lists Tri_vids;
	Lists Wed_vids;
	EmbeddingLists emb_lists;
	Egonets egonets;
	Lists id_lists;
	Lists old_id_lists;
	EdgeList edge_list;
	Ulong total_3_tris;
	Ulong total_3_path;
	Ulong total_4_clique;
	Ulong total_4_diamond;
	Ulong total_4_tailed_tris;
	Ulong total_4_cycle;
	Ulong total_3_star;
	Ulong total_4_path;

	template <typename EmbeddingTy = BaseEmbedding>
	inline bool is_vertexInduced_automorphism(unsigned n, const EmbeddingTy& emb, unsigned idx, VertexId src, VertexId dst) {
		// the new vertex id should be larger than the first vertex id
		if (dst <= emb.get_vertex(0)) return true;
		// the new vertex should not already exist in the embedding
		for (unsigned i = 1; i < n; ++i)
			if (dst == emb.get_vertex(i)) return true;
		// the new vertex should not already be extended by any previous vertex in the embedding
		for (unsigned i = 0; i < idx; ++i)
			if (is_connected(emb.get_vertex(i), dst)) return true;
		// the new vertex id should be larger than any vertex id after its source vertex in the embedding
		for (unsigned i = idx+1; i < n; ++i)
			if (dst < emb.get_vertex(i)) return true;
		return false;
	}
	inline unsigned find_motif_pattern_id_dfs(unsigned n, unsigned idx, VertexId dst, const VertexEmbedding& emb, unsigned previous_pid) {
		unsigned pid = 0;
		if (n == 2) { // count 3-motifs
			pid = 1; // 3-chain
			if (idx == 0) {
				if (is_connected(emb.get_vertex(1), dst)) pid = 0; // triangle
			}
		} else if (n == 3) { // count 4-motifs
			unsigned num_edges = 1;
			if (previous_pid == 0) { // extending a triangle
				for (unsigned j = idx+1; j < n; j ++)
					if (is_connected(emb.get_vertex(j), dst)) num_edges ++;
				pid = num_edges + 2; // p3: tailed-triangle; p4: diamond; p5: 4-clique
			} else { // extending a 3-chain
				assert(previous_pid == 1);
				std::vector<bool> connected(3, false);
				connected[idx] = true;
				for (unsigned j = idx+1; j < n; j ++) {
					if (is_connected(emb.get_vertex(j), dst)) {
						num_edges ++;
						connected[j] = true;
					}
				}
				if (num_edges == 1) {
					pid = 0; // p0: 3-path
					unsigned center = 1;
					center = is_connected(emb.get_vertex(1), emb.get_vertex(2)) ? 1 : 0;
					if (idx == center) pid = 1; // p1: 3-star
				} else if (num_edges == 2) {
					pid = 2; // p2: 4-cycle
					unsigned center = 1;
					center = is_connected(emb.get_vertex(1), emb.get_vertex(2)) ? 1 : 0;
					if (connected[center]) pid = 3; // p3: tailed-triangle
				} else {
					pid = 4; // p4: diamond
				}
			}
		} else { // count 5-motif and beyond
			find_motif_pattern_id_eigen(n, idx, dst, emb);
		}
		return pid;
	}
	inline void solve_graphlet_equations(unsigned deg_v, unsigned deg_u, Ulong tri_count, Ulong w_local_count) {
		Ulong star3_count = deg_v - tri_count - 1;
		star3_count = star3_count + deg_u - tri_count - 1;
		accumulators[4] += (tri_count * (tri_count - 1) / 2); // diamond
		accumulators[0] += tri_count * star3_count; // tailed_triangles
		accumulators[3] += (deg_v - tri_count - 1) * (deg_u - tri_count - 1); // 4-path
		accumulators[1] += (deg_v - tri_count - 1) * (deg_v - tri_count - 2) / 2; // 3-star
		accumulators[1] += (deg_u - tri_count - 1) * (deg_u - tri_count - 2) / 2;
	}
	inline void cycle(Ulong w_local_count, UintList &W_u, Ulong &cycle4_count, UintList &ind) {
		for (Ulong j = 0; j < w_local_count; j++) {
			auto src = W_u[j];
			auto begin = graph->edge_begin(src);
			auto end = graph->edge_end(src);
			for (auto e = begin; e < end; e ++) {
				auto dst = graph->getEdgeDst(e);
				if (ind[dst] == 1) cycle4_count++;
			}
			W_u[j] = 0;
		}
	}
	/*
	* @param tri_count is the total triangles centered at (v,u)
	* @param T_vu is an array containing all the third vertex of each triangle
	* @param ind is the perfect hash table for checking in O(1) time if edge (triangle, etc) exists
	*/
	inline void clique(Ulong tri_count, UintList &T_vu, Ulong &clique4_count, UintList &ind) {
		for (Ulong tr_i = 0; tr_i < tri_count; tr_i++) {
			auto src = T_vu[tr_i];
			auto begin = graph->edge_begin(src);
			auto end = graph->edge_end(src);
			for (auto e = begin; e < end; e ++) {
				auto dst = graph->getEdgeDst(e);
				if (ind[dst] == 3) clique4_count++;
			}
			ind[src] = 0;
			T_vu[tr_i] = 0;
		}
	}
	inline void mark_neighbors(VertexId &v, VertexId &u, UintList &ind) {
		auto begin = graph->edge_begin(v);
		auto end = graph->edge_end(v);
		for (auto e = begin; e < end; e ++) {
			auto w = graph->getEdgeDst(e);
			if (u == w) continue;
			ind[w] = 1;
		}
	}
	inline void triangles_and_wedges(VertexId &v, VertexId &u, UintList &T_vu, Ulong &tri_count, UintList &W_u, Ulong &w_local_count, UintList &ind) {
		auto begin = graph->edge_begin(u);
		auto end = graph->edge_end(u);
		for (auto e = begin; e < end; e ++) {
			auto w = graph->getEdgeDst(e);
			if (w == v) continue;
			if (ind[w] == 1) {
				ind[w] = 3;
				T_vu[tri_count] = w;
				tri_count++;
			}
			else {
				W_u[w_local_count] = w;
				w_local_count++;
				ind[w] = 2;
			}
		}
	}
	inline void reset_perfect_hash(VertexId src, UintList &ind) {
		auto begin = graph->edge_begin(src);
		auto end = graph->edge_end(src);
		for (auto e = begin; e < end; e ++) {
			auto dst = graph->getEdgeDst(e);
			ind[dst] = 0;
		}
	}
};

#endif
