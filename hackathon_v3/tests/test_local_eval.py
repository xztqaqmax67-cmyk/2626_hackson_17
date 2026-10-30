from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from local_eval.model import Graph, Program, build, plan_view
from local_eval.memory import insert_spills
from local_eval.simulator import audit, simulate
from local_eval.runner import evaluate

CAP = {'L1': 4096, 'UB': 4096}


def graph(ops, tensors=(), edges=()):
    return {'ops': [dict(id=n, op=name, pipe=pipe, cycles=cycles) for n, name, pipe, cycles in ops],
            'tensors': [dict(id=n, pos=pos, size=size) for n, pos, size in tensors],
            'edges': [dict(source=a, target=b) for a, b in edges]}


def chain(size=60):
    return graph([(1, 'A', 'PIPE_M', 10), (2, 'B', 'PIPE_V', 20)],
                 [(100, 'L1', size)], [(1, 100), (100, 2)])


def plan(rows):
    return {'node_to_subgraph': {str(n): n for row in rows for n in row}, 'core_schedules': rows}


class EventTests(unittest.TestCase):
    def test_two_equal_transfers(self):
        p = Program()
        for c in range(2):
            p.node(c, 'COPY_IN', 'PIPE_MTE2', ddr=True, size=60)
        tr, _, moved = simulate(p.nodes, 60, {0: 0, 1: 0})
        self.assertEqual(tr[0]['end'], 2)
        self.assertEqual(tr[1]['end'], 2)
        self.assertEqual(moved, 120)

    def test_unequal_transfers_redistribute_bandwidth(self):
        p = Program()
        p.node(0, 'COPY_IN', 'PIPE_MTE2', ddr=True, size=60)
        p.node(1, 'COPY_OUT', 'PIPE_MTE3', ddr=True, size=180)
        tr, _, _ = simulate(p.nodes, 60, {0: 0, 1: 0})
        self.assertEqual(tr[0]['end'], 2)
        self.assertEqual(tr[1]['end'], 4)

    def test_mid_transfer_arrival(self):
        p = Program()
        a = p.node(0, 'COPY_IN', 'PIPE_MTE2', ddr=True, size=180)
        b = p.node(1, 'WAIT', 'PIPE_V', cycles=1)
        c = p.node(1, 'COPY_OUT', 'PIPE_MTE3', ddr=True, size=60)
        c.depend(b.id)
        tr, _, _ = simulate(p.nodes, 60, dict.fromkeys(p.nodes, 0))
        self.assertEqual(tr[c.id]['start'], 1)
        self.assertEqual(tr[c.id]['end'], 3)
        self.assertEqual(tr[a.id]['end'], 4)

    def test_zero_duration(self):
        p = Program()
        a = p.node(0, 'COPY_IN', 'PIPE_MTE2', ddr=True, size=0)
        b = p.node(0, 'OP', 'PIPE_M', cycles=0)
        b.depend(a.id)
        tr, _, moved = simulate(p.nodes, 60, {0: 0, 1: 1})
        self.assertEqual(tr[b.id]['end'], 0)
        self.assertEqual(moved, 0)

    def test_fixed_pipe_head_cannot_be_skipped(self):
        p = Program()
        wait = p.node(1, 'WAIT', 'PIPE_M', cycles=10)
        a = p.node(0, 'A', 'PIPE_M', cycles=2)
        b = p.node(0, 'B', 'PIPE_M', cycles=1)
        a.depend(wait.id)
        b.depend(a.id)
        tr, _, _ = simulate(p.nodes, 60, {0: 0, 1: 0, 2: 1})
        self.assertEqual(tr[b.id]['start'], 12)

    def test_small_transfers_after_large_clock_do_not_stall(self):
        p = Program()
        p.node(0, 'COPY_IN', 'PIPE_MTE2', ddr=True, size=1000000000)
        prev = p.node(1, 'WAIT', 'PIPE_M', cycles=1000000000)
        for i in range(50):
            n = p.node(1, 'COPY_IN', 'PIPE_MTE2', ddr=True, size=13 + i)
            n.depend(prev.id)
            prev = n
        tr, _, _ = simulate(p.nodes, 60, {n: n for n in p.nodes})
        self.assertEqual(len(tr), 52)
        self.assertAlmostEqual(tr[prev.id]['end'], 1000000000 + sum(range(13, 63)) / 60, places=4)


class GraphTests(unittest.TestCase):
    def test_producer_consumer_sync(self):
        r = evaluate(chain(), plan([[1], [2]]), CAP, 60, 1000, trace_output=True)
        starts = {n['origin']: n['start'] for n in r['timeline'] if n['origin']}
        self.assertEqual(starts[2], 1010)
        self.assertEqual(r['makespan'], 1030)
        self.assertEqual(r['total_ddr_bytes'], 120)

    def test_copy_chain_sync_is_distinct(self):
        r = evaluate(chain(), plan([[1], [2]]), CAP, 60, 1000, 'copy_chain')
        self.assertEqual(r['makespan'], 1032)

    def test_transfer_can_dominate_sync(self):
        r = evaluate(chain(1200), plan([[1], [2]]), CAP, 60, 5)
        self.assertEqual(r['makespan'], 70)

    def test_same_core_no_sync_or_cross_copy(self):
        r = evaluate(chain(), plan([[1, 2]]), CAP, 60, 1000)
        self.assertEqual(r['makespan'], 30)
        self.assertEqual(r['total_ddr_bytes'], 0)

    def test_pipe_fix_uses_cycles(self):
        raw = graph([(1, 'L1_COPY_UB', 'PIPE_FIX', 17)])
        r = evaluate(raw, None, CAP, 60, 1000)
        self.assertEqual(r['makespan'], 17)
        self.assertEqual(r['total_ddr_bytes'], 0)

    def test_boundary_negative_copy_cycles(self):
        raw = graph([(1, 'COPY_IN', 'PIPE_MTE2', -1), (2, 'OP', 'PIPE_M', 5)],
                    [(100, 'DDR', 60), (101, 'L1', 60)], [(100, 1), (1, 101), (101, 2)])
        r = evaluate(raw, None, CAP, 60, 1000)
        self.assertEqual(r['makespan'], 6)
        self.assertEqual(r['total_ddr_bytes'], 60)

    def test_negative_compute_cycles_rejected(self):
        with self.assertRaises(ValueError):
            Graph(graph([(1, 'OP', 'PIPE_M', -1)]))

    def test_missing_op_rejected(self):
        with self.assertRaises(ValueError):
            plan_view(Graph(chain()), plan([[1]]))

    def test_bad_core_order_rejected(self):
        with self.assertRaises(ValueError):
            plan_view(Graph(chain()), plan([[2, 1]]))

    def test_cycle_rejected(self):
        raw = chain()
        raw['edges'].append({'source': 2, 'target': 1})
        with self.assertRaises(ValueError):
            Graph(raw)

    def test_multiple_writers_must_share_core(self):
        raw = graph([(1, 'A', 'PIPE_M', 1), (2, 'B', 'PIPE_M', 1), (3, 'C', 'PIPE_V', 1)],
                    [(100, 'L1', 60)], [(1, 100), (2, 100), (100, 3)])
        with self.assertRaisesRegex(ValueError, 'writers'):
            evaluate(raw, plan([[1], [2, 3]]), CAP, 60, 1000)
        self.assertTrue(evaluate(raw, plan([[1, 2], [3]]), CAP, 60, 1000)['audit_passed'])

    def test_one_writeback_shared_by_destinations(self):
        raw = chain()
        raw['ops'].append(dict(id=3, op='C', pipe='PIPE_V', cycles=20))
        raw['edges'].append(dict(source=100, target=3))
        r = evaluate(raw, plan([[1], [2], [3]]), CAP, 60, 1000)
        self.assertEqual(r['total_ddr_bytes'], 180)

    def test_operand_footprint_over_capacity_rejected(self):
        raw = graph([(1, 'OP', 'PIPE_M', 1)], [(100, 'L1', 4097)], [(1, 100)])
        with self.assertRaisesRegex(ValueError, 'simultaneous'):
            evaluate(raw, None, CAP, 60, 1000)

    def test_fixed_baseline_matches_one_subgraph(self):
        raw = chain()
        r = evaluate(raw, None, CAP, 60, 1000)
        s = evaluate(raw, {'node_to_subgraph': {'1': 0, '2': 0}, 'core_schedules': [[0]]}, CAP, 60, 1000)
        self.assertEqual(r['makespan'], s['makespan'])

    def test_late_mte3_compute_cannot_block_earlier_cross_copy_out(self):
        raw = graph([(1, 'A', 'PIPE_M', 100), (2, 'B', 'PIPE_M', 1),
                     (3, 'UB_COPY_L1', 'PIPE_MTE3', 1)],
                    [(100, 'L1', 60), (101, 'L1', 60)],
                    [(1, 100), (100, 2), (2, 101), (101, 3)])
        r = evaluate(raw, plan([[1, 3], [2]]), CAP, 60, 1000)
        self.assertTrue(r['audit_passed'])
        self.assertEqual(r['makespan'], 2102)


class MemoryTests(unittest.TestCase):
    def test_clean_backing_avoids_repeated_writeback(self):
        p = Program()
        t, other = (0, 100), (0, 101)
        p.buffers = {t: ('L1', 60), other: ('L1', 60)}
        nodes = [p.node(0, 'COPY_IN', 'PIPE_MTE2', ddr=True, size=60, outputs={t}),
                 p.node(0, 'WRITE', 'PIPE_M', cycles=1, outputs={other}),
                 p.node(0, 'READ', 'PIPE_M', cycles=1, inputs={t}),
                 p.node(0, 'READ', 'PIPE_M', cycles=1, inputs={other}),
                 p.node(0, 'READ', 'PIPE_M', cycles=1, inputs={t})]
        epochs, seqs = insert_spills(p, [[n.id for n in nodes]], {'L1': 60, 'UB': 60})
        self.assertFalse(any(n.category == 'spill_out' and t in n.inputs for n in p.nodes.values()))
        self.assertEqual(sum(n.ddr * n.size for n in p.nodes.values()), 300)
        tr, order, moved = simulate(p.nodes, 60, {n: i for i, n in enumerate(seqs[0])})
        self.assertTrue(audit(p, epochs, tr, order, {'L1': 60, 'UB': 60}, 60, moved)['audit_passed'])

    def test_repeated_partial_write_invalidates_backing(self):
        p = Program()
        t, other = (0, 100), (0, 101)
        p.buffers = {t: ('L1', 60), other: ('L1', 60)}
        nodes = [p.node(0, 'WRITE', 'PIPE_M', cycles=1, outputs={t}),
                 p.node(0, 'WRITE', 'PIPE_V', cycles=1, outputs={other}),
                 p.node(0, 'PARTIAL_WRITE', 'PIPE_M', cycles=1, outputs={t}),
                 p.node(0, 'READ', 'PIPE_V', cycles=1, inputs={other}),
                 p.node(0, 'READ', 'PIPE_M', cycles=1, inputs={t})]
        epochs, seqs = insert_spills(p, [[n.id for n in nodes]], {'L1': 60, 'UB': 60})
        # t is evicted twice, and its second write must force another writeback.
        writes = [n for n in p.nodes.values() if n.category == 'spill_out' and t in n.inputs]
        reads = [n for n in p.nodes.values() if n.category == 'spill_in' and t in n.outputs]
        self.assertEqual(len(writes), 2)
        self.assertEqual(len(reads), 2)
        self.assertIn(reads[0].id, nodes[2].deps)
        priorities = {n: i for i, n in enumerate(seqs[0])}
        tr, order, moved = simulate(p.nodes, 60, priorities)
        self.assertTrue(audit(p, epochs, tr, order, {'L1': 60, 'UB': 60}, 60, moved)['audit_passed'])

    def test_audit_rejects_modified_timeline(self):
        p = Program()
        a = p.node(0, 'A', 'PIPE_M', cycles=2)
        b = p.node(1, 'B', 'PIPE_V', cycles=3)
        b.depend(a.id, 1000)
        tr, order, moved = simulate(p.nodes, 60, {0: 0, 1: 0})
        tr[b.id]['start'] = 2
        with self.assertRaises(ValueError):
            audit(p, [], tr, order, CAP, 60, moved)


if __name__ == '__main__':
    unittest.main()
