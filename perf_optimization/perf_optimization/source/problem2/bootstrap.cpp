// 对应 bootstrap.py。
// 快速建立首个完整方案，让限时搜索尽早拥有可保存的有效结果。
#include "bootstrap.hpp"
namespace problem2 {
Plan first_plan(const GraphInfo &g, Id cores) {
    Plan p;
    p.ncores = cores;
    Id count = Id(g.id_order.size());
    if (!count) {
        return p;
    }
    for (Id k = 0; k < std::min(cores, count); ++k) {
        p.blocks[k];
    }
    std::size_t largest = 0;
    for (const auto &atom : g.atoms) {
        largest = std::max(largest, atom.size());
    }
    // 小连通分量可整组分配；单个分量太大时才按拓扑顺序切成连续区间。
    if (Id(g.atoms.size()) >= cores && largest <= double(count) / cores * 1.5) {
        std::vector<Id> load(cores);
        std::map<Id, Id> owners;
        auto atoms = g.atoms;
        std::stable_sort(atoms.begin(), atoms.end(), [](const Ids &a, const Ids &b) {
            return std::make_pair(-Id(a.size()), a.front()) <
                   std::make_pair(-Id(b.size()), b.front());
        });
        for (const auto &atom : atoms) {
            Id target = Id(std::min_element(load.begin(), load.end()) - load.begin());
            load[target] += Id(atom.size());
            for (Id op : atom) {
                owners[op] = target;
            }
        }
        for (Id op : g.id_order) {
            p.blocks[owners.at(op)].push_back(op);
        }
    } else {
        Id n = Id(p.blocks.size());
        for (Id i = 0; i < count; ++i) {
            p.blocks[i * n / count].push_back(g.id_order[i]);
        }
    }
    for (auto i = p.blocks.begin(); i != p.blocks.end();) {
        if (i->second.empty()) {
            i = p.blocks.erase(i);
        } else {
            p.core_of[i->first] = i->first;
            p.order.push_back(i->first);
            ++i;
        }
    }
    p.next_block_id = p.blocks.rbegin()->first + 1;
    p.validate(g);
    return p;
}
} // namespace problem2
