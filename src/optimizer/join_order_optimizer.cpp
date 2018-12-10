#include "optimizer/join_order_optimizer.hpp"
#include "planner/operator/list.hpp"
#include "parser/expression/list.hpp"

using namespace duckdb;
using namespace std;

using Relation = JoinOrderOptimizer::Relation;
using RelationSet = JoinOrderOptimizer::RelationSet;
using RelationInfo = JoinOrderOptimizer::RelationInfo;
using EdgeInfo = JoinOrderOptimizer::EdgeInfo;
using JoinNode = JoinOrderOptimizer::JoinNode;

//! Returns true if A and B are disjoint, false otherwise
template<class T>
static bool Disjoint(unordered_set<T>& a, unordered_set<T>& b) {
	for(auto &entry : a) {
		if (b.find(entry) != b.end()) {
			return false;
		}
	}
	return true;
}

//! Extract the set of relations referred to inside an expression
void JoinOrderOptimizer::ExtractBindings(Expression &expression, unordered_set<size_t> &bindings) {
	if (expression.type == ExpressionType::COLUMN_REF) {
		auto &colref = (ColumnRefExpression&) expression;
		assert(colref.binding.table_index != (size_t) -1);
		// map the binding using the mapping used by the JoinOrderOptimizer
		assert(relation_mapping.find(colref.binding.table_index) != relation_mapping.end());
		bindings.insert(relation_mapping[colref.binding.table_index]);
	}
	for(auto &child : expression.children) {
		ExtractBindings(*child, bindings);
	}
}

static void PrintEdgeSet(unordered_map<size_t, EdgeInfo>& tree, vector<size_t> prefix = {}) {
	for(auto &entry : tree) {
		if (entry.second.neighbors.size() > 0) {
			string source = "[";
			for(auto pr : prefix) {
				source += to_string(pr) + ", ";
			}
			source += to_string(entry.first) + "]";
			for(auto &neighbor : entry.second.neighbors) {
				auto dest = neighbor->ToString();
				fprintf(stderr, "%s -> %s\n", source.c_str(), dest.c_str());
			}
		}
	}
}

static void PrintPlans(unordered_map<RelationSet*, unique_ptr<JoinNode>>& plans) {
	for(auto &node : plans) {
		auto relation_str = node.first->ToString();
		fprintf(stderr, "%s - [Cost %zu][Estimated Cardinality: %zu]\n", relation_str.c_str(), node.second->cost, node.second->cardinality);
	}
}

// FIXME: don't get just the LogicalGet, get everything underneath any join (i.e. JOIN(FILTER(GET), FILTER(GET)) should return the two FILTER nodes)
// FIXME: also get everything that happens BEFORE the first join (i.e. LIMIT(JOIN(...))) should store the LIMIT as well, because this will still be the root node after the reordering
// FIXME: should take Filter etc into account when reordering
bool JoinOrderOptimizer::ExtractJoinRelations(LogicalOperator &input_op) {
	LogicalOperator *op = &input_op;
	while(op->children.size() == 1) {
		if (op->type == LogicalOperatorType::FILTER) {
			// extract join conditions from filter
			for(auto &f : op->expressions) {
				if (f->GetExpressionClass() == ExpressionClass::COMPARISON) {
					// comparison, can be used as join condition
					filters.push_back(f.get());
				}
			}
		}
		op = op->children[0].get();
	}
	if (op->type == LogicalOperatorType::SUBQUERY || 
		op->type == LogicalOperatorType::TABLE_FUNCTION) {
		// not supported yet!
		return false;
	}
	if (op->type == LogicalOperatorType::JOIN) {
		if (((LogicalJoin*)op)->type != JoinType::INNER) {
			// non-inner join not supported yet
			return false;
		}
		// extract join conditions
		for(auto &f : op->expressions) {
			if (f->GetExpressionClass() == ExpressionClass::COMPARISON) {
				// comparison, can be used as join condition
				filters.push_back(f.get());
			}
		}

	}
	if (op->type == LogicalOperatorType::JOIN ||
	    op->type == LogicalOperatorType::CROSS_PRODUCT) {
		// join or cross product
		if (!ExtractJoinRelations(*op->children[0])) {
			return false;
		}
		if (!ExtractJoinRelations(*op->children[1])) {
			return false;
		}
		return true;
	} else if (op->type == LogicalOperatorType::GET) {
		// base table scan, add to set of relations
		auto get = (LogicalGet*) op;
		Relation r;
		r.index = get->table_index;
		r.op = &input_op;
		relations[r.index] = r;
		return true;
	}
	return false;
}

RelationSet *JoinOrderOptimizer::GetRelation(unique_ptr<size_t[]> relations, size_t count) {
	// now look it up in the tree
	auto *node = &relation_set;
	RelationInfo *info = nullptr;
	for(size_t i = 0; i < count; i++) {
		auto entry = node->find(relations[i]);
		if (entry == node->end()) {
			// node not found, create it
			auto insert_it = node->insert(make_pair(relations[i], RelationInfo()));
			entry = insert_it.first;
		}
		// move to the next node
		info = &entry->second;
		node = &info->children;
	}
	assert(info);
	// now check if the RelationSet has already been created
	if (!info->relation) {
		// if it hasn't we need to create it
		info->relation = make_unique<RelationSet>(move(relations), count);
	}
	return info->relation.get();
}

//! Create or get a RelationSet from a single node with the given index
RelationSet *JoinOrderOptimizer::GetRelation(size_t index) {
	// create a sorted vector of the relations
	auto relations = unique_ptr<size_t[]>(new size_t[1]);
	relations[0] = index;
	size_t count = 1;
	return GetRelation(move(relations), count);
}

RelationSet* JoinOrderOptimizer::GetRelation(unordered_set<size_t> &bindings) {
	assert(bindings.size() > 0);
	// create a sorted vector of the relations
	auto relations = unique_ptr<size_t[]>(new size_t[bindings.size()]);
	size_t count = 0;
	for(auto &entry : bindings) {
		relations[count++] = entry;
	}
	sort(relations.get(), relations.get() + count);
	return GetRelation(move(relations), count);
}

//! Create a RelationSet that is the union of the left and right relations
RelationSet* JoinOrderOptimizer::Union(RelationSet *left, RelationSet *right) {
	auto relations = unique_ptr<size_t[]>(new size_t[left->count + right->count]);
	size_t count = 0;
	// move through the left and right relations, eliminating duplicates
	size_t i = 0, j = 0;
	while(true) {
		if (i == left->count) {
			// exhausted left relation, add remaining of right relation
			for(; j < right->count; j++) {
				relations[count++] = right->relations[j];
			}
			break;
		} else if (j == right->count) {
			// exhausted right relation, add remaining of left
			for(; i < left->count; i++) {
				relations[count++] = left->relations[i];
			}
			break;
		} else if (left->relations[i] == right->relations[j]) {
			// equivalent, add only one of the two pairs
			relations[count++] = left->relations[i];
			i++;
			j++;
		} else if (left->relations[i] < right->relations[j]) {
			// left is smaller, progress left and add it to the set
			relations[count++] = left->relations[i];
			i++;
		} else {
			// right is smaller, progress right and add it to the set
			relations[count++] = right->relations[j];
			j++;
		}
	}
	return GetRelation(move(relations), count);
}

EdgeInfo* JoinOrderOptimizer::GetEdgeInfo(RelationSet *left) {
	assert(left && left->count > 0);
	// find the EdgeInfo corresponding to the left set
	auto *node = &edge_set;
	EdgeInfo *info = nullptr;
	for(size_t i = 0; i < left->count; i++) {
		auto entry = node->find(left->relations[i]);
		if (entry == node->end()) {
			// node not found, create it
			auto insert_it = node->insert(make_pair(left->relations[i], EdgeInfo()));
			entry = insert_it.first;
		}
		// move to the next node
		info = &entry->second;
		node = &info->children;
	}
	assert(info);
	return(info);
}

void JoinOrderOptimizer::CreateEdge(RelationSet *left, RelationSet *right) {
	assert(left && right && left->count > 0 && right->count > 0);
	// find the EdgeInfo corresponding to the left set
	auto info = GetEdgeInfo(left);
	// now insert the edge to the right child
	info->neighbors.push_back(right);
}

//! Create a new JoinTree node by joining together two previous JoinTree nodes
static unique_ptr<JoinNode> CreateJoinTree(RelationSet *set, JoinNode *left, JoinNode *right) {
	// for the hash join we want the right side (build side) to have the smallest cardinality
	// also just a heuristic but for now...
	// FIXME: we should probably actually benchmark that as well
	// FIXME: should consider different join algorithms, should we pick a join algorithm here as well? (probably)
	if (left->cardinality < right->cardinality) {
		return CreateJoinTree(set, right, left);
	}
	// the expected cardinality is the max of the child cardinalities
	// FIXME: we should obviously use better cardinality estimation here
	// but for now we just assume foreign key joins only
	size_t expected_cardinality = std::max(left->cardinality, right->cardinality);
	// cost is expected_cardinality plus the cost of the previous plans
	size_t cost = expected_cardinality + left->cost + right->cost;
	fprintf(stderr, "%s - %s [Cost: %zu][Cardinality: %zu]\n", left->set->ToString().c_str(), right->set->ToString().c_str(), cost, expected_cardinality);
	return make_unique<JoinNode>(set, left, right, expected_cardinality, cost);
}

//! Returns true if a RelationSet is banned by the list of exclusion_set, false otherwise
static bool RelationSetIsExcluded(RelationSet *node, unordered_set<size_t> &exclusion_set) {
	for(size_t i = 0; i < node->count; i++) {
		if (exclusion_set.find(node->relations[i]) != exclusion_set.end()) {
			return true;
		}
	}
	return false;
}

//! Update the exclusion set with all entries in the subgraph
static void UpdateExclusionSet(RelationSet *node, unordered_set<size_t> &exclusion_set) {
	for(size_t i = 0; i < node->count; i++) {
		exclusion_set.insert(node->relations[i]);
	}
}

void JoinOrderOptimizer::EnumerateNeighbors(RelationSet *node, function<bool(RelationSet*)> callback) {
	auto *edges = &edge_set;
	for(size_t i = 0; i < node->count; i++) {
		auto entry = edges->find(node->relations[i]);
		if (entry == edges->end()) {
			// node not found
			return;
		}
		// check if any subset of the other set is in this sets neighbors
		auto info = &entry->second;
		for(auto neighbor : info->neighbors) {
			if (callback(neighbor)) {
				return;
			}
		}

		// move to the next node
		edges = &info->children;
	}
}

vector<size_t> JoinOrderOptimizer::GetNeighbors(RelationSet *node, unordered_set<size_t> &exclusion_set) {
	vector<size_t> result;
	EnumerateNeighbors(node, [&](RelationSet *neighbor) -> bool {
		if (!RelationSetIsExcluded(neighbor, exclusion_set)) {
			// add the smallest node of the neighbor to the set
			result.push_back(neighbor->relations[0]);
		}
		return false;
	});
	return result;
}

//! Returns true if sub is a subset of super
static bool IsSubset(RelationSet *super, RelationSet *sub) {
	if (sub->count > super->count) {
		return false;
	}
	size_t j = 0;
	for(size_t i = 0; i < super->count; i++) {
		if (sub->relations[j] == super->relations[i]) {
			j++;
			if (j == sub->count) {
				return true;
			}
		}
	}
	return false;
}

bool JoinOrderOptimizer::IsConnected(RelationSet *node, RelationSet *other) {
	bool is_connected = false;
	EnumerateNeighbors(node, [&](RelationSet *neighbor) -> bool {
		if (IsSubset(other, neighbor)) {
			is_connected = true;
			return true;
		}
		return false;
	});
	return is_connected;
}

void JoinOrderOptimizer::EmitPair(RelationSet *left, RelationSet *right) {
	// get the left and right join plans
	auto &left_plan = plans[left];
	auto &right_plan = plans[right];
	auto new_set = Union(left, right);
	// create the join tree based on combining the two plans
	auto new_plan = CreateJoinTree(new_set, left_plan.get(), right_plan.get());
	// check if this plan is the optimal plan we found for this set of relations
	auto entry = plans.find(new_set);
	if (entry == plans.end() || new_plan->cost < entry->second->cost) {
		plans[new_set] = move(new_plan);
	}
}

void JoinOrderOptimizer::EmitCSG(RelationSet *node) {
	// create the exclusion set as everything inside the subgraph AND anything with members BELOW it
	unordered_set<size_t> exclusion_set;
	for(size_t i = 0; i < node->relations[0]; i++) {
		exclusion_set.insert(i);
	}
	UpdateExclusionSet(node, exclusion_set);
	// find the neighbors given this exclusion set
	auto neighbors = GetNeighbors(node, exclusion_set);
	if (neighbors.size() == 0) {
		return;
	}
	// we iterate over the neighbors ordered by their first node
	sort(neighbors.begin(), neighbors.end());
	for(auto neighbor : neighbors) {
		// since the GetNeighbors only returns the smallest element in a list, the entry might not be connected to (only!) this neighbor,  hence we have to do a connectedness check before we can emit it
		auto neighbor_relation = GetRelation(neighbor);
		if (IsConnected(node, neighbor_relation)) {
			EmitPair(node, neighbor_relation);
		}
		EnumerateCmpRecursive(node, neighbor_relation, exclusion_set);
	}
}

void JoinOrderOptimizer::EnumerateCmpRecursive(RelationSet *left, RelationSet *right, unordered_set<size_t> exclusion_set) {
	// get the neighbors of the second relation under the exclusion set
	auto neighbors = GetNeighbors(right, exclusion_set);
	if (neighbors.size() == 0) {
		return;
	}
	vector<RelationSet*> union_sets;
	union_sets.resize(neighbors.size());
	for(size_t i = 0; i < neighbors.size(); i++) {
		auto neighbor = GetRelation(neighbors[i]);
		// emit the combinations of this node and its neighbors
		auto combined_set = Union(right, neighbor);
		if (plans.find(combined_set) != plans.end() &&
			IsConnected(left, combined_set)) {
			EmitPair(left, combined_set);
		}
		union_sets[i] = combined_set;
		// updated the set of excluded entries with this neighbor
		exclusion_set.insert(neighbors[i]);
	}
	// recursively enumerate the sets, with the new exclusion sets
	for(size_t i = 0; i < neighbors.size(); i++) {
		EnumerateCmpRecursive(left, union_sets[i], exclusion_set);
	}
}

void JoinOrderOptimizer::EnumerateCSGRecursive(RelationSet *node, unordered_set<size_t> &exclusion_set) {
	// find neighbors of S under the exlusion set
	auto neighbors = GetNeighbors(node, exclusion_set);
	if (neighbors.size() == 0) {
		return;
	}
	// now first emit the connected subgraphs of the neighbors
	vector<RelationSet*> union_sets;
	union_sets.resize(neighbors.size());
	for(size_t i = 0; i < neighbors.size(); i++) {
		auto neighbor = GetRelation(neighbors[i]);
		// emit the combinations of this node and its neighbors
		auto new_set = Union(node, neighbor);
		if (plans.find(new_set) != plans.end()) {
			EmitCSG(new_set);
		}
		union_sets[i] = new_set;
		// updated the set of excluded entries with this neighbor
		exclusion_set.insert(neighbors[i]);
	}
	// recursively enumerate the sets, with the new exclusion sets
	for(size_t i = 0; i < neighbors.size(); i++) {
		EnumerateCSGRecursive(union_sets[i] , exclusion_set);
	}
}

// the join ordering is pretty much a straight implementation of the paper "Dynamic Programming Strikes Back" by Guido Moerkotte and Thomas Neumannn, see that paper for additional info/documentation
// bonus slides: https://db.in.tum.de/teaching/ws1415/queryopt/chapter3.pdf?lang=de
// FIXME: this should also do filter pushdown
// FIXME: incorporate cardinality estimation into the plans, possibly by pushing samples?
unique_ptr<LogicalOperator> JoinOrderOptimizer::Optimize(unique_ptr<LogicalOperator> plan) {
	// first extract a list of all relations that have to be joined together
	// and a list of all conditions that is applied to them
	if (!ExtractJoinRelations(*plan)) {
		// do not support reordering this type of plan
		return plan;
	}
	if (relations.size() <= 1) {
		// at most one relation, nothing to reorder
		return plan;
	}
	// create the relation mapping of index (0-n) -> Relation
	size_t index = 0;
	for(auto &kv : relations) {
		relation_mapping[index++] = kv.first;
	}
	// create potential edges from the comparisons
	for(auto &filter : filters) {
		auto comparison = (ComparisonExpression*) filter;
		// extract the bindings that are required for the left and right side of the comparison
		unordered_set<size_t> left_bindings, right_bindings;
		ExtractBindings(*comparison->children[0], left_bindings);
		ExtractBindings(*comparison->children[1], right_bindings);
		if (left_bindings.size() > 0 && right_bindings.size() > 0) {
			// both the left and the right side have bindings
			// check if they are disjoint
			if (Disjoint(left_bindings, right_bindings)) {
				// they are disjoint, create the edges in the join graph
				// first create the relation sets, if they do not exist
				RelationSet *left_set  = GetRelation(left_bindings);
				RelationSet *right_set = GetRelation(right_bindings);

				// now add the edges to the edge set
				CreateEdge(left_set, right_set);
				CreateEdge(right_set, left_set);
			} else {
				// FIXME: they are not disjoint, but maybe they can still be pushed down?
			}
		} else {
			// FIXME: this comparison can be pushed down into a base relation
			// as only one side has a set of bindings
		}
	}
	PrintEdgeSet(edge_set);
	// now use dynamic programming to figure out the optimal join order
	// note: we can just use pointers to RelationSet* here because the CreateRelation/GetRelation function ensures that a unique combination of relations will have a unique RelationSet object
	// initialize each of the single-node plans with themselves and with their cardinalities
	// these are the leaf nodes of the join tree
	for(size_t i = 0; i < relations.size(); i++) {
		auto &rel = relations[relation_mapping[i]];
		auto node = GetRelation(i);
		plans[node] = make_unique<JoinNode>(node, rel.op->EstimateCardinality());
	}
	// now we perform the actual dynamic programming to compute the final result
	// we enumerate over all the possible pairs in the neighborhood
	for(size_t i = relations.size(); i > 0; i--) {
		// for every node in the set, we consider it as the start node once
		auto start_node = GetRelation(i - 1);
		// emit the start node
		EmitCSG(start_node);
		// initialize the set of exclusion_set as all the nodes with a number below this
		unordered_set<size_t> exclusion_set;
		for(size_t j = 0; j < i - 1; j++) {
			exclusion_set.insert(j);
		}
		// then we recursively search for neighbors that do not belong to the banned entries
		EnumerateCSGRecursive(start_node, exclusion_set);
	}
	PrintPlans(plans);
	// now the optimal join path should have been found
	// get it from the node
	unordered_set<size_t> bindings;
	for(size_t i = 0; i < relations.size(); i++) {
		bindings.insert(i);
	}
	auto total_relation = GetRelation(bindings);
	assert(plans.find(total_relation) != plans.end());

	



	throw NotImplementedException("Join order optimization!");
}
