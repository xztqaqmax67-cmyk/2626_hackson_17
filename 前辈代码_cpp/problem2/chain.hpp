#pragma once
#include "pipeline.hpp"

namespace problem2 {
// 路径是不相交的真实依赖链；segments 仅用于共同分核，不作为缩点后的子图。
struct ChainDecomposition {
    std::vector<Ids> paths, segments;
    std::map<Id, Id> segment_of;
};
ChainDecomposition chain_decompose(const GraphInfo &graph, Id cores, const Settings &settings,
                                    double segment_scale = .5, double communication_weight = 0,
                                    bool fill_unmatched = false, double deadline = 0);
// 固定参数网格产生纯链初解，按内部估价预筛；不包含旧初解、重排搜索或迁移搜索。
// limit=0 返回全部不重复且通过内存防护的链候选，供开发阶段独立完整评估。
NamedPlans chain_seed_plans(const GraphInfo &graph, Id cores, const Settings &settings,
                            double deadline = 0, Id limit = 8);
} // namespace problem2
