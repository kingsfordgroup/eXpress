/**
 *  rhotree.cpp
 *  express
 *
 *  Created by Adam Roberts on 9/17/12.
 *
 **/

#include "rhotree.h"

#include "main.h"
#include "fragments.h"
#include "targets.h"

#include <boost/algorithm/string.hpp>
#include <boost/tokenizer.hpp>

using namespace std;

Sap Sap::branch(size_t split) {
  size_t old_left = _l;
  vector<size_t>::iterator it = upper_bound(_params->leaf_ids.begin() + _l,
                                            _params->leaf_ids.begin() + _r + 1,
                                            split);
  _l = it - _params->leaf_ids.begin();
  return Sap(_params, old_left, _l-1);
}

double RhoTree::similarity_scalar(const Sap& sap) {
  return LOG_1;
  if (sap.size() < _children.size()) {
    return LOG_1;
  }
  double c = log(sap.size());
  double tot = sap.total_const_likelihood();
  if (tot == LOG_0) {
    return LOG_0;
  }
  for (size_t i = 0; i < sap.size(); ++i) {
    double p = sap.const_likelihood(i) - tot;
    c += sexp(p)*p;
    assert(!isnan(c));
  }
  if (c < 0) {
    //    assert(approx_eq(c,0));
    return LOG_0;
  }
  if (c > 1) {
    return LOG_1;
  }
  assert(c==0 || !isnan(log(c)));
  return log(c);
}

RangeRhoForest::RangeRhoForest(string infile_path, double ff_param)
    : RangeRhoTree(0, 0, ff_param) {
  load_from_file(infile_path);
}

void RangeRhoForest::load_from_file(string infile_path) {
  ifstream infile (infile_path.c_str());
  string line;

  if (!infile.is_open()) {
    cerr << "ERROR: Could not open hierarchy specification file '"
         << infile_path << "'.";
    exit(1);
  }

  getline(infile, line, '\n');
  size_t split = line.find(',');
  size_t num_leaves = atoi(line.substr(0, split).c_str());
  size_t num_nodes = atoi(line.substr(split+1).c_str());
  _target_to_leaf_map = vector<LeafID>(num_leaves, -1);
  _leaf_to_tree_map = vector<TreeID>(num_leaves, -1);
  LeafID next_leaf_id = 0;
  vector<RangeRhoTree*> nodes(num_nodes, NULL);

  cout << "Loading hierarchy from '" << infile_path << "' with " << num_nodes
       << " nodes and " << num_leaves << " leaves...\n";
  boost::char_separator<char> sep_edges(";");

  while (infile.good()) {
    getline(infile, line, '\n');
    boost::algorithm::trim(line);
    if (line.empty()) {
      continue;
    }
    boost::tokenizer<boost::char_separator<char> > tokens(line, sep_edges);
    TreeID parent, child;
    foreach (const string& edge, tokens) {
      size_t split = edge.find(',');
      parent = atoi(edge.substr(0, split).c_str());
      child = atoi(edge.substr(split+1).c_str());
      if (child < num_leaves) {
        assert(_target_to_leaf_map[child] == -1);
        nodes[next_leaf_id] = new RangeRhoTree(next_leaf_id, next_leaf_id,
                                               _ff_param);
        _target_to_leaf_map[child] = next_leaf_id;
        _leaf_to_tree_map[next_leaf_id] = _children.size();
        child = next_leaf_id;
        next_leaf_id++;
      }
      assert(nodes[child]);
      if (!nodes[parent]) {
        nodes[parent] = new RangeRhoTree(nodes[child]->left(),
                                         nodes[child]->right(),
                                         _ff_param);
      }
      else if (nodes[parent]->right() + 1 != nodes[child]->left()) {
        cerr << "ERROR: '" << infile_path << "' does not specify a proper "
             << "hierarchy.";
        exit(1);
      }
      nodes[parent]->add_child(nodes[child]);
    }
    add_child(nodes[parent]);
  }
  
  for(TargID t = 0; t < num_leaves; ++t) {
    if (_target_to_leaf_map[t] == -1) {
      nodes[next_leaf_id] = new RangeRhoTree(next_leaf_id, next_leaf_id,
                                             _ff_param);
      _target_to_leaf_map[t] = next_leaf_id;
      _leaf_to_tree_map[next_leaf_id] = _children.size();
      add_child(nodes[next_leaf_id]);
      next_leaf_id++;
    }
  }
  
  assert(next_leaf_id == num_leaves);
  _right = num_leaves-1;
  _tree_counts = vector<size_t>(_children.size(), 0);
}

void RangeRhoForest::set_alphas(const vector<double>& target_alphas) {
  SapData params(target_alphas.size());
  assert(target_alphas.size() == num_leaves());
  for (TargID i = 0; i < target_alphas.size(); i++) {
    //    LeafID leaf = _target_to_leaf_map[i];
    LeafID leaf = i;
    params.leaf_ids[leaf] = leaf;
    params.rhos[leaf] = target_alphas[i];
    assert(params.rhos[leaf] != LOG_0);
  }
  for (LeafID i = 0; i < num_leaves(); ++i) {
    assert(params.rhos[i] != LOG_0);
    params.accum_assignments[i+1] = log_add(params.accum_assignments[i],
                                            params.rhos[i]);
  }
  set_rhos(Sap(&params));
}

void RangeRhoForest::get_rhos(Sap sap, double rho) const {
  TreeID tree = sap.tree_root();
  static_cast<RangeRhoTree*>(_children[tree])->get_rhos(sap, _child_rhos(tree));
}

void RangeRhoForest::update_rhos(Sap sap) {
  TreeID tree = sap.tree_root();

  // Don't compute a scalar here to save time. These should always be valuable.
  double mass = next_mass();
  // Update the rhos throughout the forest using the computed likelihoods.
  assert(approx_eq(sap.fraction(), LOG_1));
  _child_rhos.increment(tree, mass);
  static_cast<RangeRhoTree*>(_children[tree])->update_rhos(sap);
}

void RangeRhoForest::process_fragment(const Fragment& frag) {
  // Assume the likelihoods are pre-computed in probability field.

  // Everything is much easier and faster when the mapping is unique.
  if (frag.num_hits() == 1) {
    SapData params(1);
    FragHit& hit = *frag[0];
    params.leaf_ids[0] = hit.targ_id;
    params.tree_root = _leaf_to_tree_map[params.leaf_ids[0]];
    params.accum_assignments[1] = 0;
    double mass = next_mass();
    _child_rhos.increment(params.tree_root, mass);
    static_cast<RangeRhoTree*>(_children[params.tree_root])->update_rhos(Sap(&params, 0, 0));
    _tree_counts[params.tree_root]++;
    return;
  }

  SapData params(frag.num_hits());

  for (size_t i = 0; i < frag.num_hits(); ++i) {
    FragHit& hit = *frag[i];
    params.leaf_ids[i] = hit.targ_id;
    params.const_likelihoods[i] = hit.probability;
    params.accum_const_likelihoods[i+1] = log_add(params.accum_const_likelihoods[i],
                                                 hit.probability);
    if (i == 0) {
      params.tree_root = _leaf_to_tree_map[params.leaf_ids[i]];
    } else {
      assert(_leaf_to_tree_map[params.leaf_ids[i]] == params.tree_root);
      if (_leaf_to_tree_map[params.leaf_ids[i]] != params.tree_root) {
        cerr << "ERROR: Invalid hierarchy forest. Alignment of '"
             << frag.name() << "' accesses multiple trees.";
        exit(1);
      }
    }
  }

  Sap sap(&params);
  get_rhos(sap, LOG_1);

  // Compute the individual and total likelihoods.
  double total_likelihood = LOG_0;
  for (size_t i = 0; i < frag.num_hits(); ++i) {
    total_likelihood = log_add(total_likelihood,
                               params.const_likelihoods[i] + params.rhos[i]);
  }

  // Accumulate the fractional assignments in the Sap for easy lookup later on.
  for (size_t i = 0; i < frag.num_hits(); ++i) {
    double frac = params.const_likelihoods[i] + params.rhos[i] -
                  total_likelihood;
    frag[i]->probability = frac;
    params.accum_assignments[i+1] = log_add(frac, params.accum_assignments[i]);
  }
  
  update_rhos(sap);
  _tree_counts[params.tree_root]++;
}

RangeRhoTree::RangeRhoTree(size_t left, size_t right, double ff_param)
    : RhoTree(ff_param), _left(left), _right(right) {
}

void RangeRhoTree::add_child(RangeRhoTree* child) {
  if (_children.empty()) {
    _left = child->left();
    _right = child->right();
  } else {
    assert(child->left() == _right + 1);
    _right = child->right();
  }
  RhoTree::add_child(child);
}

void RangeRhoTree::set_rhos(Sap sap) {
  _child_rhos = FrequencyMatrix<double>(1, _children.size(), LOG_0);
  for (size_t i = 0; i < _children.size(); ++i) {
    RangeRhoTree& child = *static_cast<RangeRhoTree*>(_children[i]);
    Sap branch_sap = sap.branch(child.right());
    if (branch_sap.size()) {
      child.set_rhos(branch_sap);
      _child_rhos.increment(i, branch_sap.fraction());
      assert(!isnan(_child_rhos(i)));
    }
    if (sap.size() == 0) {
      break;
    }
  }
}

void RangeRhoTree::get_rhos(Sap sap, double rho) const {
  assert(!isnan(rho));
  if (is_leaf()) {
    for (size_t i = 0; i < sap.size(); ++i) {
      sap.rho(i) = rho;
    }
    return;
  }

  for (size_t i = 0; i < _children.size(); ++i) {
    RangeRhoTree& child = *static_cast<RangeRhoTree*>(_children[i]);
    Sap branch_sap = sap.branch(child.right());
    if (branch_sap.size()) {
      child.get_rhos(branch_sap, rho + _child_rhos(i));
    }
    if (sap.size() == 0) {
      break;
    }
  }
}

void RangeRhoTree::update_rhos(Sap sap) {
  if (is_leaf()) {
    return;
  }
  const double mass = next_mass() + similarity_scalar(sap);
  if (islzero(mass)) {
    return;
  }
  
  for (size_t i = 0; i < _children.size(); ++i) {
    RangeRhoTree& child = *static_cast<RangeRhoTree*>(_children[i]);
    Sap branch_sap = sap.branch(child.right());
    if (branch_sap.size()) {
      child.update_rhos(branch_sap);
      _child_rhos.increment(i, mass + branch_sap.fraction());
    }
    if (sap.size() == 0) {
      break;
    }
  }
}
