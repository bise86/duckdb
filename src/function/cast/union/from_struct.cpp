#include "duckdb/function/cast/bound_cast_data.hpp"

namespace duckdb {

bool StructToUnionCast::AllowImplicitCastFromStruct(const LogicalType &source, const LogicalType &target) {
	if (source.id() != LogicalTypeId::STRUCT) {
		return false;
	}
	auto member_count = UnionType::GetMemberCount(target);
	auto fields = StructType::GetChildTypes(source);
	if (member_count != fields.size()) {
		// Struct should have the same amount of fields as the union has members
		return false;
	}
	for (idx_t i = 0; i < member_count; i++) {
		auto &member = UnionType::GetMemberType(target, i);
		auto &member_name = UnionType::GetMemberName(target, i);
		auto &field = fields[i].second;
		auto &field_name = fields[i].first;
		if (member != field) {
			return false;
		}
		if (member_name != field_name) {
			return false;
		}
	}
	return true;
}

// Physical Cast execution

void ReconstructTagVector(Vector &result, idx_t count) {
	auto &type = result.GetType();
	auto member_count = UnionType::GetMemberCount(type);

	for (idx_t i = 0; i < count; i++) {
		auto
	}
}

bool StructToUnionCast::Cast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<StructBoundCastData>();
	auto &lstate = parameters.local_state->Cast<StructCastLocalState>();

	D_ASSERT(source.GetType().id() == LogicalTypeId::STRUCT);
	D_ASSERT(result.GetType().id() == LogicalTypeId::UNION);
	D_ASSERT(cast_data.target.id() == LogicalTypeId::UNION);

	auto &source_children = StructVector::GetEntries(source);
	D_ASSERT(source_children.size() == UnionType::GetMemberCount(result.GetType()));

	bool all_converted = true;
	for (idx_t i = 0; i < source_children.size(); i++) {
		auto &result_child_vector = UnionVector::GetMember(result, i);
		auto &source_child_vector = *source_children[i];
		CastParameters child_parameters(parameters, cast_data.child_cast_info[i].cast_data, lstate.local_states[i]);
		if (!cast_data.child_cast_info[i].function(source_child_vector, result_child_vector, count, child_parameters)) {
			all_converted = false;
		}
	}
	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, ConstantVector::IsNull(source));
	} else {
		source.Flatten(count);
		FlatVector::Validity(result) = FlatVector::Validity(source);
	}
	return all_converted;
}

// Bind cast

unique_ptr<BoundCastData> StructToUnionCast::BindData(BindCastInput &input, const LogicalType &source,
                                                      const LogicalType &target) {
	vector<BoundCastInfo> child_cast_info;
	D_ASSERT(source.id() == LogicalTypeId::STRUCT);
	D_ASSERT(target.id() == LogicalTypeId::UNION);

	auto source_child_count = StructType::GetChildCount(source);
	auto result_child_count = UnionType::GetMemberCount(target);
	D_ASSERT(source_child_count == result_child_count);

	for (idx_t i = 0; i < result_child_count; i++) {
		auto &source_child = StructType::GetChildType(source, i);
		auto &target_child = UnionType::GetMemberType(target, i);

		auto child_cast = input.GetCastFunction(source_child, target_child);
		child_cast_info.push_back(std::move(child_cast));
	}
	return make_uniq<StructBoundCastData>(std::move(child_cast_info), target);
}

BoundCastInfo StructToUnionCast::Bind(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	auto cast_data = StructToUnionCast::BindData(input, source, target);
	return BoundCastInfo(&StructToUnionCast::Cast, std::move(cast_data), StructToUnionCast::InitLocalState);
}

// Initialize local state

unique_ptr<FunctionLocalState> StructToUnionCast::InitLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<StructBoundCastData>();
	auto result = make_uniq<StructCastLocalState>();

	for (auto &entry : cast_data.child_cast_info) {
		unique_ptr<FunctionLocalState> child_state;
		if (entry.init_local_state) {
			CastLocalStateParameters child_params(parameters, entry.cast_data);
			child_state = entry.init_local_state(child_params);
		}
		result->local_states.push_back(std::move(child_state));
	}
	return std::move(result);
}

} // namespace duckdb
