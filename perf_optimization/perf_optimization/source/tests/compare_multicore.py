"""小型 DAG 覆盖场景 B 跨核搬运、直接依赖、空核及搜索邻域的完整记录。"""
import random
from compare_original import *
from problem2 import official, search, activation


def fixture(seed, count=12):
    rng = random.Random(seed)
    ops = [dict(id=100+i, op='COMPUTE', pipe=rng.choice(['PIPE_V', 'PIPE_M']),
                cycles=rng.randrange(0, 120)) for i in range(count)]
    tensors, edges = [], []
    for i in range(count+1):
        tid = 1000+i
        tensors.append(dict(id=tid, pos='UB', size=rng.choice([0, 16, 32, 64])))
        if i:
            edges.append(dict(source=99+i, target=tid))
        targets = [i] if i < count else []
        targets += [j for j in range(i+1, count) if rng.random() < .15]
        for j in targets:
            edges.append(dict(source=tid, target=100+j))
    for i in range(count-2):
        if rng.random() < .4:
            edges.append(dict(source=100+i, target=102+i, data_size=rng.choice([0, 8, 32])))
    return dict(ops=ops, tensors=tensors, edges=edges)


def branch_fixture(seed, count=24):
    rng = random.Random(seed)
    graph = fixture(seed, count)
    graph['edges'] = []
    graph['tensors'] = [dict(id=1000+i, pos='UB', size=rng.choice([0, 16, 32, 64]))
                        for i in range(count+4)]
    for i in range(count):
        graph['edges'] += [dict(source=1000+i, target=100+i),
                           dict(source=100+i, target=1004+i)]
        if i >= 4 and rng.random() < .25:
            source = (i//4-1)*4 + (i+1)%4
            graph['edges'].append(dict(source=100+source, target=100+i, data_size=8))
    # 原程序依赖部分插入顺序；打乱记录顺序可检查 C++ 是否意外按 ID 排序。
    rng.shuffle(graph['ops'])
    rng.shuffle(graph['tensors'])
    return graph


def main():
    for seed in range(20):
        print(f'small multicore seed={seed}', flush=True)
        graph = fixture(seed) if seed < 12 else branch_fixture(seed)
        if seed >= 16:
            for index, op in enumerate(graph['ops']):
                if index % 3 == 0:
                    op['pipe'] = 'PIPE_FIX'
                    op['op'] = 'L1_LOOP_ENHANCE'
        plan = generate_multicore_plan(graph, num_cores=3, seed=seed, min_subgraph_size=1,
                                      max_subgraph_size=3)
        compare('generate', graph, plan, cores=3, seed=seed, min_size=1, max_size=3)
        settings = dict(capacity=CAPACITY, bandwidth=7, cross_core_copy_delay=13)
        compare('boundary', graph, official.build_boundary_graphs(graph, plan, 7), plan=plan, bandwidth=7)
        result = evaluate_scene_b(graph, plan, 7, CAPACITY, 13)
        compare('scene_b', graph, result, plan=plan, bandwidth=7, delay=13)
        profile = official.build_profile(graph, plan, settings, result)
        compare('profile', graph, profile, plan=plan, bandwidth=7, delay=13)
        info = core.GraphInfo(graph)
        native = core.Plan.from_dict(info, plan)
        for family in range(3):
            candidates = search.neighborhoods(info, native, profile, result, family, settings=settings)
            expected = [dict(kind=c.kind, label=c.label, plan=c.plan.to_dict(), needs_repair=c.needs_repair)
                        for c in candidates]
            compare('neighborhoods', graph, expected, plan=plan, bandwidth=7, delay=13, family=family)
        one = core.single_core_plan(info, 3)
        expected = [(label, p.to_dict()) for label, p in activation.activation_plans(info, one, settings)]
        compare('activation', graph, expected, plan=one.to_dict(), bandwidth=7, delay=13)


if __name__ == '__main__':
    main()
