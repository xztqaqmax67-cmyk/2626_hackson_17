#include "core.hpp"
#include "search.hpp"
#include "official_code/multicore_cut_evaluate_problem_2.hpp"
#include "utils.hpp"

using namespace problem2;
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("perf_runner request.json");
        auto request = read_json(path_from_utf8(argv[1]));
        double start = monotonic();
        auto graph = read_json(path_from_utf8(request.at("graph").get<std::string>()));
        std::string action = request.at("action");
        Id cores = request.value("cores", 4);
        auto settings = submission_settings();
        Json summary = {{"action",action}};
        Json plan;
        if (action == "evaluate") {
            plan = read_json(path_from_utf8(request.at("plan").get<std::string>()));
        } else {
            GraphInfo g(graph);
            summary["prepare_seconds"] = monotonic()-start;
            if (action == "search") {
                SearchOptions options;
                options.time_limit = request.value("budget", 20.);
                options.max_evaluations = request.value("max_evaluations", 0);
                options.compact = true;
                options.export_results = false;
                Json report;
                double begin = monotonic();
                plan = solve_in_memory(g, cores, options, &report);
                summary["solve_seconds"] = monotonic()-begin;
                summary["report"] = std::move(report);
                if (!plan.is_null()) atomic_write(path_from_utf8(request.at("output").get<std::string>()), plan);
                summary["plan_available"] = !plan.is_null();
                summary["total_seconds"] = monotonic()-start;
                std::cout << summary.dump() << '\n';
                return plan.is_null() ? 5 : 0;
            }
            Plan p;
            p.ncores = cores;
            if (action == "single") {
                p = single_core_plan(g, cores);
            } else {
                p.order = g.id_order;
                p.next_block_id = g.ids.empty() ? 0 : g.ids.back()+1;
                for (std::size_t i=0;i<g.id_order.size();++i) {
                    Id op=g.id_order[i];
                    p.blocks[op]={op};
                    p.core_of[op]=Id(i*cores/g.id_order.size());
                }
            }
            p.validate(g);
            double begin=monotonic();
            plan=p.to_dict();
            summary["to_dict_seconds"]=monotonic()-begin;
            begin=monotonic();
            summary["fingerprint"]=p.fingerprint();
            summary["fingerprint_seconds"]=monotonic()-begin;
            begin=monotonic();
            summary["route_bytes"]=route_bytes(g,p);
            summary["route_seconds"]=monotonic()-begin;
            begin=monotonic();
            summary["sketch"]=replay_sketch(g,p,settings);
            summary["sketch_seconds"]=monotonic()-begin;
            begin=monotonic();
            auto lifetime=lifetime_proxy(g,p,settings.capacity);
            summary["lifetime"]={lifetime.first,lifetime.second};
            summary["lifetime_seconds"]=monotonic()-begin;
            if (request.contains("output")) atomic_write(path_from_utf8(request.at("output").get<std::string>()),plan);
        }
        double begin=monotonic();
        auto result=evaluate_scene_b(graph,plan,settings.bandwidth,settings.capacity,settings.cross_core_copy_delay);
        summary["evaluation_seconds"]=monotonic()-begin;
        summary["makespan"]=result.at("makespan");
        summary["movement"]=result.at("data_movement_bytes");
        nlohmann::json canonical=result;
        summary["result_sha256"]=sha256_text(canonical.dump());
        summary["total_seconds"]=monotonic()-start;
        std::cout << summary.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cout << Json{{"error",error.what()}}.dump() << '\n';
        return 1;
    }
}
