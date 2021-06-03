#include "dsdgen_helpers.hpp"

#define DECLARER
#include "address.hpp"
#include "build_support.hpp"
#include "config.hpp"
#include "dist.hpp"
#include "genrand.hpp"
#include "params.hpp"
#include "porting.hpp"
#include "scaling.hpp"
#include "tdefs.hpp"

namespace tpcds {

void InitializeDSDgen(int scale) {
	char scale_str[12];
	sprintf(scale_str, "%d", scale);
	set_int("SCALE", scale_str); // set SF, which also does a default init (e.g. random seed)
	init_rand();                 // no random numbers without this
}

ds_key_t GetRowCount(int table_id) {
	return get_rowcount(table_id);
}

void ResetCountCount() {
	resetCountCount();
}

tpcds_table_def GetTDefByNumber(int table_id) {
	auto tdef = getSimpleTdefsByNumber(table_id);
	tpcds_table_def def;
	def.name = tdef->name;
	def.fl_child = tdef->flags & FL_CHILD ? 1 : 0;
	def.fl_small = tdef->flags & FL_SMALL ? 1 : 0;
	return def;
}

tpcds_builder_func GetTDefFunctionByNumber(int table_id) {
	auto table_funcs = getTdefFunctionsByNumber(table_id);
	return table_funcs->builder;
}

} // namespace tpcds
