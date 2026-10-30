#pragma once
#include "common.hpp"
namespace problem2 {
using Blocks = std::map<Id, Ids>;
using PipeLoads = std::vector<std::map<std::string, double>>;
// 只保存真实数据依赖：两个算子仅仅共享输入，不代表它们之间有先后约束。
struct GraphInfo {
    Json graph;
    Records ops, tensors;
    Ids ids, id_order;
    IdSet eligible;
    Adjacency preds, succs, producers, consumers, eligible_producers, eligible_consumers,
        external_input_groups, original_output_relations;
    std::map<Id, Ids> op_inputs, op_outputs;
    std::vector<Json> direct_edges;
    std::map<Id, std::vector<std::tuple<Id, Id, Id>>> direct_inputs;
    std::map<Id, Id> weight, rank, position, atom_of;
    std::map<std::pair<Id, Id>, Id> edge_bytes;
    std::vector<Ids> atoms;
    explicit GraphInfo(const Json &input);
    static GraphInfo from_path(const fs::path &path);
    Ids topological_order(const std::string &mode = "locality") const;
};
struct Plan {
    // blocks: 块 ID -> 算子 ID 列表；core_of: 块 ID -> 所属核心。
    // order 是满足依赖的全局块顺序，导出时各核心从中提取自己的 FIFO 顺序。
    Blocks blocks;
    std::map<Id, Id> core_of;
    Ids order;
    Id next_block_id = 0, ncores = 1; // 分裂块时分配新 ID；ncores 包含空闲核心。
    std::map<Id, Id> owner() const;
    Json to_dict() const;
    std::string fingerprint() const;
    bool validate(const GraphInfo &graph) const;
    static Plan from_dict(const GraphInfo &graph, const Json &data);
};
std::pair<Adjacency, Adjacency> block_dependencies(const GraphInfo &graph, const Blocks &blocks);
Ids topological_blocks(const Adjacency &preds, const Adjacency &succs,
                       const std::map<Id, double> &priority = {});
Blocks build_blocks(const GraphInfo &graph, Id cores, Id max_ops = 64, double deadline = 0);
Plan single_core_plan(const GraphInfo &graph, Id cores);
Json route_details(const GraphInfo &graph, const Plan &plan);
Id route_bytes(const GraphInfo &graph, const Plan &plan);
PipeLoads pipe_loads(const GraphInfo &graph, const Plan &plan);
// 五流水线草图只用于候选排序；它不模拟换页和共享 DDR，不能代替官方评估。
class Sketch {
    const GraphInfo *graph_;
    const Settings *settings_;
    std::map<std::string, double> copy_out_, copy_in_;
    std::map<std::pair<Id, Id>, double> local_ready_;
    std::set<std::pair<Id, Id>> output_done_;

  public:
    PipeLoads available, fixed;
    std::map<Id, double> finish;
    std::map<Id, Id> core;
    Id route_bytes = 0;
    Sketch(const GraphInfo &graph, Id cores, const Settings &settings);
    double transfer(const std::string &route, Id size, Id source, Id target, double ready);
    double tensor_ready(Id tid, Id target);
    double append(const Ids &nodes, Id target);
    double makespan() const;
};
Plan construct(const GraphInfo &graph, Id cores, double lambda, const Settings &settings,
               const std::optional<Blocks> &blocks = {}, double deadline = 0);
Json replay_sketch_details(const GraphInfo &graph, const Plan &plan, const Settings &settings,
                           double deadline = 0);
double replay_sketch(const GraphInfo &graph, const Plan &plan, const Settings &settings,
                     double deadline = 0);
Plan migrate(const Plan &plan, const Ids &blocks, Id target);
Plan swap(const Plan &plan, Id first, Id second);
std::optional<Plan> split_block(const GraphInfo &graph, const Plan &plan, Id block,
                                std::optional<Id> cut = {});
std::optional<Plan> merge_adjacent(const GraphInfo &graph, const Plan &plan, Id first,
                                   std::optional<Id> second = {}, Id max_ops = 128);
std::pair<Id, Id> insertion_bounds(const GraphInfo &graph, const Plan &plan, Id block);
std::optional<Plan> move_block(const GraphInfo &graph, const Plan &plan, Id block, Id gap);
Json lifetime_proxy_details(const GraphInfo &graph, const Plan &plan, const Capacity &capacity);
std::pair<Id, Id> lifetime_proxy(const GraphInfo &graph, const Plan &plan,
                                 const Capacity &capacity);
std::optional<Plan> repair_order(const GraphInfo &graph, const Plan &plan, const Settings &settings,
                                 const Json &profile = Json(),
                                 const std::optional<IdSet> &affected = {}, Id max_moves = 2);
} // namespace problem2
