#include "official_code/schedule_step1.hpp"
#include "official_code/schedule_step2.hpp"
#include "official_code/schedule_step3.hpp"
#include "official_code/stub_multicore_cut_and_schedule.hpp"
#include "official_code/multicore_cut_evaluate_problem_2.hpp"
#include "official_code/evaluation_validation.hpp"
#include "core.hpp"
#include "bootstrap.hpp"
#include "chain.hpp"
#include "pipeline.hpp"
#include "structure.hpp"
#include "activation.hpp"
#include "official.hpp"
#include "search.hpp"
// 测试专用 JSON 协议：同一输入分别送给 Python 原版和 C++ 版，比较完整结果。
int main() {
    using namespace problem2;
    try {
        Json request = Json::parse(std::cin), result;
        auto graph = request.at("graph");
        auto action = request.at("action").get<std::string>();
        if (action == "step1") {
            result = step1_schedule(graph);
        } else if (action == "generate") {
            result = generate_multicore_plan(
                graph, request.value("cores", 4), request.value("seed", std::uint64_t(0)),
                request.value("min_size", 50), request.value("max_size", 100));
        } else {
            Capacity capacity = request.at("capacity").get<Capacity>();
            double bandwidth = request.value("bandwidth", 60.);
            if (action == "chain_seeds" || action == "legacy_seeds") {
                GraphInfo g(graph);
                Settings cfg{capacity, bandwidth, request.value("delay", 1000.)};
                const Id cores = request.value("cores", 4);
                const Id limit = request.value("limit", 8);
                if (cores < 2 || cores > 5 || limit < 1 || limit > 64) {
                    throw std::invalid_argument("invalid seed benchmark cores/limit");
                }
                const double began = monotonic();
                NamedPlans seeds;
                if (action == "chain_seeds") {
                    seeds = chain_seed_plans(g, cores, cfg, 0, limit);
                } else {
                    append(seeds, pipeline_seed_plans(g, cores, cfg));
                    append(seeds, structure_seed_plans(g, cores, cfg));
                    for (double lambda : {0., .2, 1.}) {
                        seeds.emplace_back("native_lambda_" + std::to_string(lambda),
                                           construct(g, cores, lambda, cfg));
                    }
                }
                Json rows = array();
                std::set<std::string> seen;
                for (const auto &[label, plan] : seeds) {
                    if (!seen.insert(plan.fingerprint()).second) continue;
                    plan.validate(g);
                    const Id traffic = route_bytes(g, plan);
                    const double sketch = replay_sketch(g, plan, cfg);
                    const auto memory = lifetime_proxy(g, plan, cfg.capacity);
                    const double average_excess = double(memory.first) /
                        std::max<std::size_t>(1, plan.order.size());
                    const double cost = std::max(sketch, traffic / cfg.bandwidth) +
                        .15 * traffic / cfg.bandwidth + 2 * average_excess / cfg.bandwidth;
                    rows.push_back({{"label", label}, {"plan", plan.to_dict()},
                        {"proxy", {{"sketch", sketch}, {"route_bytes", traffic},
                            {"lifetime_excess", memory.first}, {"lifetime_peak", memory.second},
                            {"cost", cost}}}});
                    if (Id(rows.size()) >= limit) break;
                }
                result = {{"seeds", rows}, {"construction_seconds", monotonic() - began}};
            } else if (action == "core") {
                GraphInfo g(graph);
                Settings cfg{capacity, bandwidth, 500};
                Id cores = request.value("cores", 3);
                auto p = first_plan(g, cores);
                NamedPlans seeds = structure_seed_plans(g, cores, cfg);
                append(seeds, pipeline_seed_plans(g, cores, cfg));
                Json rows = array();
                for (const auto &[label, plan] : seeds) {
                    rows.push_back({label, plan.to_dict()});
                }
                auto native = construct(g, cores, 0, cfg);
                result = {{"id_order", g.id_order},
                          {"locality", g.topological_order()},
                          {"critical", g.topological_order("critical")},
                          {"atoms", g.atoms},
                          {"bootstrap", p.to_dict()},
                          {"construct", native.to_dict()},
                          {"fingerprint", native.fingerprint()},
                          {"route", route_details(g, native)},
                          {"sketch", replay_sketch_details(g, native, cfg)},
                          {"seeds", rows}};
            } else if (action == "boundary") {
                result = official::build_boundary_graphs(graph, request.at("plan"), bandwidth);
            } else if (action == "profile") {
                Settings cfg{capacity, bandwidth, request.value("delay", 500.)};
                auto score = evaluate_scene_b(graph, request.at("plan"), bandwidth, capacity,
                                              cfg.cross_core_copy_delay);
                result = official::build_profile(graph, request.at("plan"), cfg, score);
            } else if (action == "neighborhoods") {
                Settings cfg{capacity, bandwidth, request.value("delay", 500.)};
                GraphInfo g(graph);
                auto plan = Plan::from_dict(g, request.at("plan"));
                auto score = evaluate_scene_b(graph, request.at("plan"), bandwidth, capacity,
                                              cfg.cross_core_copy_delay);
                auto profile = official::build_profile(graph, request.at("plan"), cfg, score);
                result = array();
                for (const auto &candidate :
                     neighborhoods(g, plan, profile, score, request.value("family", 0), cfg)) {
                    result.push_back({{"kind", candidate.kind},
                                      {"label", candidate.label},
                                      {"plan", candidate.plan.to_dict()},
                                      {"needs_repair", candidate.needs_repair}});
                }
            } else if (action == "activation") {
                Settings cfg{capacity, bandwidth, request.value("delay", 500.)};
                GraphInfo g(graph);
                auto plan = Plan::from_dict(g, request.at("plan"));
                result = array();
                for (const auto &[label, candidate] : activation_plans(g, plan, cfg)) {
                    result.push_back({label, candidate.to_dict()});
                }
            } else if (action == "solve_memory") {
                // 与原 solve 使用同一预算比较方案，检查正式入口没有丢失算法步骤。
                SearchOptions options;
                options.time_limit = request.value("time_limit", 120.);
                options.max_evaluations = request.value("max_evaluations", 12);
                options.compact = request.value("compact", false);
                options.ablation = request.value("ablation", std::string("full"));
                options.export_results = false;
                result = solve_in_memory(GraphInfo(graph), request.value("cores", 3), options);
            } else if (action == "solve") {
                SearchOptions options;
                options.time_limit = request.value("time_limit", 120.);
                options.max_evaluations = request.value("max_evaluations", 12);
                options.export_results = false;
                options.compact = request.value("compact", false);
                options.ablation = request.value("ablation", std::string("full"));
                result = solve(path_from_utf8(request.at("graph_path").get<std::string>()),
                               request.value("cores", 3),
                               path_from_utf8(request.at("config_path").get<std::string>()),
                               path_from_utf8(request.at("output").get<std::string>()), options);
            } else if (action == "scene_b") {
                result = evaluate_scene_b(graph, request.at("plan"), bandwidth, capacity,
                                          request.value("delay", 500.));
            } else if (action == "step3") {
                result = step3_simulation(graph, capacity, bandwidth);
            } else if (action == "step2") {
                result = step2_spill_insertion(graph, request.at("sequence").get<Ids>(), capacity);
            } else {
                throw std::runtime_error("unknown action");
            }
        }
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cout << problem2::Json{{"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
