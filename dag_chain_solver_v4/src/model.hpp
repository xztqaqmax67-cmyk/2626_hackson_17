#pragma once
// Adapted from the repository's hackathon_v3 solver (not an official evaluator).
// Fixed Scene B constants; COPY_OUT -> delay -> COPY_IN; per-destination writes.
// All costs here are heuristic estimates. No real tensor data is moved.
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
using namespace std;
namespace fs = std::filesystem;
using i64 = int64_t;
using Clock = chrono::steady_clock;
using Row = array<double, 5>;
constexpr double INF = 1e100;

// Small, strict JSON reader. IDs and byte counts remain signed 64-bit integers.
// Accepts ordinary JSON, including unknown fields; required graph fields are checked below.
struct Json {
    enum Type { Null, Boolean, Number, String, Array, Object } type = Null;
    string text;
    vector<Json> items;
    vector<pair<string, Json>> fields;
    const Json &at(const string &key) const {
        if (type != Object)
            throw runtime_error("expected JSON object for " + key);
        for (const auto &p : fields)
            if (p.first == key)
                return p.second;
        throw runtime_error("missing JSON field: " + key);
    }
    const Json *find(const string &key) const {
        for (const auto &p : fields)
            if (p.first == key)
                return &p.second;
        return nullptr;
    }
    string str() const {
        if (type != String)
            throw runtime_error("expected JSON string");
        return text;
    }
    i64 integer() const {
        if (type != Number || text.find_first_of(".eE") != string::npos)
            throw runtime_error("expected integer JSON value");
        size_t n = 0;
        i64 value = stoll(text, &n);
        if (n != text.size())
            throw runtime_error("invalid integer");
        return value;
    }
    const vector<Json> &array() const {
        if (type != Array)
            throw runtime_error("expected JSON array");
        return items;
    }
};
struct JsonParser {
    const string &s;
    size_t p = 0;
    explicit JsonParser(const string &s_) : s(s_) {}
    [[noreturn]] void fail(const string &msg) const {
        throw runtime_error("JSON at byte " + to_string(p) + ": " + msg);
    }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t'))
            ++p;
    }
    unsigned hex4() {
        if (p + 4 > s.size())
            fail("short Unicode escape");
        unsigned x = 0;
        for (int j = 0; j < 4; ++j) {
            char c = s[p++];
            x *= 16;
            if (c >= '0' && c <= '9')
                x += c - '0';
            else if (c >= 'a' && c <= 'f')
                x += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                x += c - 'A' + 10;
            else
                fail("invalid Unicode escape");
        }
        return x;
    }
    static void utf8(string &t, unsigned x) {
        if (x < 128)
            t += char(x);
        else if (x < 2048) {
            t += char(192 | (x >> 6));
            t += char(128 | (x & 63));
        } else if (x < 65536) {
            t += char(224 | (x >> 12));
            t += char(128 | ((x >> 6) & 63));
            t += char(128 | (x & 63));
        } else {
            t += char(240 | (x >> 18));
            t += char(128 | ((x >> 12) & 63));
            t += char(128 | ((x >> 6) & 63));
            t += char(128 | (x & 63));
        }
    }
    string string_value() {
        if (p >= s.size() || s[p++] != '"')
            fail("expected string");
        string t;
        while (p < s.size()) {
            unsigned char c = s[p++];
            if (c == '"')
                return t;
            if (c < 32)
                fail("unescaped control character");
            if (c != '\\') {
                t += char(c);
                continue;
            }
            if (p == s.size())
                fail("short escape");
            char e = s[p++];
            switch (e) {
            case '"':
                t += '"';
                break;
            case '\\':
                t += '\\';
                break;
            case '/':
                t += '/';
                break;
            case 'b':
                t += '\b';
                break;
            case 'f':
                t += '\f';
                break;
            case 'n':
                t += '\n';
                break;
            case 'r':
                t += '\r';
                break;
            case 't':
                t += '\t';
                break;
            case 'u': {
                unsigned x = hex4();
                if (x >= 0xD800 && x <= 0xDBFF) {
                    if (p + 2 > s.size() || s[p] != '\\' || s[p + 1] != 'u')
                        fail("missing low surrogate");
                    p += 2;
                    unsigned y = hex4();
                    if (y < 0xDC00 || y > 0xDFFF)
                        fail("invalid low surrogate");
                    x = 65536 + ((x - 0xD800) << 10) + (y - 0xDC00);
                } else if (x >= 0xDC00 && x <= 0xDFFF)
                    fail("unexpected low surrogate");
                utf8(t, x);
                break;
            }
            default:
                fail("invalid escape");
            }
        }
        fail("unterminated string");
    }
    Json value(int depth = 0) {
        if (depth > 256)
            fail("excessive nesting");
        ws();
        if (p == s.size())
            fail("unexpected end");
        Json v;
        char c = s[p];
        if (c == '"') {
            v.type = Json::String;
            v.text = string_value();
            return v;
        }
        if (c == '[') {
            v.type = Json::Array;
            ++p;
            ws();
            if (p < s.size() && s[p] == ']') {
                ++p;
                return v;
            }
            while (true) {
                v.items.push_back(value(depth + 1));
                ws();
                if (p == s.size())
                    fail("unterminated array");
                char q = s[p++];
                if (q == ']')
                    break;
                if (q != ',')
                    fail("expected comma");
            }
            return v;
        }
        if (c == '{') {
            v.type = Json::Object;
            ++p;
            ws();
            if (p < s.size() && s[p] == '}') {
                ++p;
                return v;
            }
            unordered_set<string> keys;
            while (true) {
                ws();
                string k = string_value();
                if (!keys.insert(k).second)
                    fail("duplicate key: " + k);
                ws();
                if (p == s.size() || s[p++] != ':')
                    fail("expected colon");
                v.fields.emplace_back(move(k), value(depth + 1));
                ws();
                if (p == s.size())
                    fail("unterminated object");
                char q = s[p++];
                if (q == '}')
                    break;
                if (q != ',')
                    fail("expected comma");
            }
            return v;
        }
        for (auto literal : {string("true"), string("false"), string("null")}) {
            if (s.compare(p, literal.size(), literal) == 0) {
                p += literal.size();
                v.type = literal == "null" ? Json::Null : Json::Boolean;
                v.text = literal;
                return v;
            }
        }
        size_t b = p;
        if (s[p] == '-')
            ++p;
        if (p == s.size() || s[p] < '0' || s[p] > '9')
            fail("expected value");
        if (s[p] == '0')
            ++p;
        else
            while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                ++p;
        if (p < s.size() && s[p] == '.') {
            ++p;
            size_t q = p;
            while (p < s.size() && isdigit((unsigned char)s[p]))
                ++p;
            if (q == p)
                fail("bad fraction");
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-'))
                ++p;
            size_t q = p;
            while (p < s.size() && isdigit((unsigned char)s[p]))
                ++p;
            if (q == p)
                fail("bad exponent");
        }
        v.type = Json::Number;
        v.text = s.substr(b, p - b);
        return v;
    }
    Json parse() {
        if (s.size() >= 3 && (unsigned char)s[0] == 239 && (unsigned char)s[1] == 187 &&
            (unsigned char)s[2] == 191)
            p = 3;
        Json v = value();
        ws();
        if (p != s.size())
            fail("trailing input");
        return v;
    }
};
inline string read_text(const fs::path &p) {
    ifstream f(p, ios::binary);
    if (!f)
        throw runtime_error("cannot open " + p.string());
    ostringstream out;
    out << f.rdbuf();
    if (f.bad())
        throw runtime_error("read failed: " + p.string());
    return out.str();
}
inline i64 nonnegative(const Json &j, const string &field) {
    i64 v = j.integer();
    if (v < 0)
        throw runtime_error(field + " must be nonnegative");
    return v;
}
inline int pipe_id(const string &s) {
    if (s == "PIPE_MTE2")
        return 0;
    if (s == "PIPE_MTE3")
        return 1;
    if (s == "PIPE_M")
        return 2;
    if (s == "PIPE_V")
        return 3;
    if (s == "PIPE_FIX")
        return 4;
    throw runtime_error("unsupported pipe: " + s);
}
struct Config {
    array<i64, 2> capacity{{1572864, 131072}};
    double bandwidth = 60, delay = 1000;
    static constexpr int spill_trigger_percent = 90;
    i64 spill_limit(int memory) const {
        // Integer floor, without overflowing the intermediate multiplication.
        return capacity[memory] / 100 * spill_trigger_percent +
               capacity[memory] % 100 * spill_trigger_percent / 100;
    }
};
struct Op {
    i64 id = 0, cycles = 0;
    int pipe = 0;
    string name;
    vector<int> inputs, outputs, pred, succ, direct_in, direct_inc;
};
struct Tensor {
    i64 id = 0, size = 0;
    int pos = 0;
    vector<int> producers, consumers;
    bool final_output = false;
};
struct Direct {
    int a, b;
    i64 size;
};
struct DSU {
    vector<int> p, sz;
    explicit DSU(int n) : p(n), sz(n, 1) { iota(p.begin(), p.end(), 0); }
    int get(int x) {
        while (x != p[x])
            x = p[x] = p[p[x]];
        return x;
    }
    void join(int a, int b) {
        a = get(a);
        b = get(b);
        if (a == b)
            return;
        if (sz[a] < sz[b])
            swap(a, b);
        p[b] = a;
        sz[a] += sz[b];
    }
};
struct Graph {
    vector<Op> ops;
    vector<Tensor> tensors;
    vector<Direct> direct;
    vector<int> topo, dfs_order, dfs_pos, level, height, component, writer_group;
    vector<double> rank, depth;
    size_t edge_count = 0;
    int components = 0;
    static void unique_vec(vector<int> &v) {
        sort(v.begin(), v.end());
        v.erase(unique(v.begin(), v.end()), v.end());
    }
    explicit Graph(const Json &root) {
        struct RawOp {
            i64 id, cycles;
            string name;
            int pipe, eligible = -1;
            vector<int> in, out;
        };
        vector<RawOp> raw;
        unordered_map<i64, int> oid, tid;
        unordered_set<i64> all;
        for (const auto &j : root.at("tensors").array()) {
            Tensor t;
            t.id = nonnegative(j.at("id"), "tensor id");
            t.size = nonnegative(j.at("size"), "size");
            string pos = j.at("pos").str();
            if (pos != "L1" && pos != "UB" && pos != "DDR")
                throw runtime_error("unknown tensor position: " + pos);
            t.pos = pos == "L1" ? 0 : 1;
            // Scene B remaps a direct DDR operand to UB, as the supplied evaluator does.
            if (!all.insert(t.id).second)
                throw runtime_error("duplicate node id");
            tid[t.id] = int(tensors.size());
            tensors.push_back(move(t));
        }
        for (const auto &j : root.at("ops").array()) {
            RawOp o;
            o.id = nonnegative(j.at("id"), "op id");
            o.name = j.at("op").str();
            o.cycles = j.at("cycles").integer();
            if (o.cycles < 0) {
                if (o.cycles != -1 || (o.name != "COPY_IN" && o.name != "COPY_OUT"))
                    throw runtime_error("negative cycles only allowed as -1 on DDR COPY ops");
                o.cycles = 0;
            }
            o.pipe = pipe_id(j.at("pipe").str());
            if (!all.insert(o.id).second)
                throw runtime_error("duplicate node id");
            raw.push_back(move(o));
        }
        sort(raw.begin(), raw.end(), [](const RawOp &a, const RawOp &b) { return a.id < b.id; });
        for (int i = 0; i < (int)raw.size(); ++i) {
            oid[raw[i].id] = i;
            if (raw[i].name != "COPY_IN" && raw[i].name != "COPY_OUT") {
                raw[i].eligible = int(ops.size());
                Op o;
                o.id = raw[i].id;
                o.cycles = raw[i].cycles;
                o.name = raw[i].name;
                o.pipe = raw[i].pipe;
                ops.push_back(move(o));
            }
        }
        vector<vector<int>> rp(tensors.size()), rc(tensors.size()), adj(raw.size());
        vector<tuple<int, int, i64>> rawdirect;
        set<pair<i64, i64>> edge_pairs;
        for (const auto &j : root.at("edges").array()) {
            i64 a = nonnegative(j.at("source"), "edge source"),
                b = nonnegative(j.at("target"), "edge target");
            if (!all.count(a) || !all.count(b))
                throw runtime_error("unknown edge endpoint");
            if (!edge_pairs.emplace(a, b).second)
                throw runtime_error("duplicate edge: " + to_string(a) + " -> " + to_string(b));
            bool ao = oid.count(a), bo = oid.count(b);
            if (ao && !bo) {
                int u = oid.at(a), t = tid.at(b);
                raw[u].out.push_back(t);
                rp[t].push_back(u);
            } else if (!ao && bo) {
                int t = tid.at(a), v = oid.at(b);
                raw[v].in.push_back(t);
                rc[t].push_back(v);
            } else if (ao && bo) {
                i64 sz = j.find("data_size") ? nonnegative(*j.find("data_size"), "data_size") : 0;
                rawdirect.emplace_back(oid.at(a), oid.at(b), sz);
            } else
                throw runtime_error("tensor-to-tensor edge is unsupported");
        }
        for (int t = 0; t < (int)tensors.size(); ++t) {
            unique_vec(rp[t]);
            unique_vec(rc[t]);
            for (int p : rp[t])
                for (int c : rc[t])
                    adj[p].push_back(c);
            for (int p : rp[t])
                if (raw[p].eligible >= 0)
                    tensors[t].producers.push_back(raw[p].eligible);
            bool copyout = false;
            for (int c : rc[t]) {
                if (raw[c].eligible >= 0)
                    tensors[t].consumers.push_back(raw[c].eligible);
                if (raw[c].name == "COPY_OUT")
                    copyout = true;
            }
            tensors[t].final_output =
                !tensors[t].producers.empty() && (copyout || tensors[t].consumers.empty());
        }
        for (auto [a, b, s] : rawdirect) {
            adj[a].push_back(b);
            int u = raw[a].eligible, v = raw[b].eligible;
            if (u >= 0 && v >= 0) {
                int d = int(direct.size());
                direct.push_back({u, v, s});
                ops[v].direct_in.push_back(d);
                ops[u].direct_inc.push_back(d);
                ops[v].direct_inc.push_back(d);
            }
        }
        vector<int> deg(raw.size());
        for (auto &row : adj) {
            unique_vec(row);
            for (int v : row)
                ++deg[v];
        }
        queue<int> q;
        for (int v = 0; v < (int)raw.size(); ++v)
            if (!deg[v])
                q.push(v);
        int count = 0;
        while (!q.empty()) {
            int v = q.front();
            q.pop();
            ++count;
            for (int w : adj[v])
                if (!--deg[w])
                    q.push(w);
        }
        if (count != (int)raw.size())
            throw runtime_error("input graph contains a cycle");
        vector<int> seen(raw.size(), -1), stack;
        for (int r = 0; r < (int)raw.size(); ++r) {
            int v = raw[r].eligible;
            if (v < 0)
                continue;
            ops[v].inputs = raw[r].in;
            ops[v].outputs = raw[r].out;
            // Keep the original edge insertion order, as GraphInfo.op_inputs/op_outputs do.
            // Sorting tensor IDs here would change the order of COPY reservations in trials.
            auto stable_unique = [](vector<int> &ids) {
                unordered_set<int> seen_ids;
                vector<int> result;
                result.reserve(ids.size());
                for (int t : ids)
                    if (seen_ids.insert(t).second)
                        result.push_back(t);
                ids.swap(result);
            };
            stable_unique(ops[v].inputs);
            stable_unique(ops[v].outputs);
            stack = adj[r];
            while (!stack.empty()) {
                int u = stack.back();
                stack.pop_back();
                if (seen[u] == r)
                    continue;
                seen[u] = r;
                int w = raw[u].eligible;
                if (w >= 0)
                    ops[v].succ.push_back(w);
                else
                    for (int z : adj[u])
                        stack.push_back(z);
            }
            unique_vec(ops[v].succ);
            for (int w : ops[v].succ)
                ops[w].pred.push_back(v);
        }
        int n = int(ops.size());
        DSU writers(n);
        for (const auto &t : tensors)
            for (size_t j = 1; j < t.producers.size(); ++j)
                writers.join(t.producers[0], t.producers[j]);
        writer_group.resize(n);
        for (int v = 0; v < n; ++v)
            writer_group[v] = writers.get(v);
        priority_queue<int, vector<int>, greater<int>> h;
        deg.assign(n, 0);
        for (int v = 0; v < n; ++v) {
            unique_vec(ops[v].pred);
            deg[v] = int(ops[v].pred.size());
            if (!deg[v])
                h.push(v);
            edge_count += ops[v].succ.size();
        }
        while (!h.empty()) {
            int v = h.top();
            h.pop();
            topo.push_back(v);
            for (int w : ops[v].succ)
                if (!--deg[w])
                    h.push(w);
        }
        if ((int)topo.size() != n)
            throw runtime_error("contracted compute graph contains a cycle");
        rank.assign(n, 0);
        depth.assign(n, 0);
        level.assign(n, 0);
        height.assign(n, 0);
        DSU ds(n);
        for (int v : topo)
            for (int w : ops[v].succ) {
                depth[w] = max(depth[w], depth[v] + weight(v));
                level[w] = max(level[w], level[v] + 1);
                ds.join(v, w);
            }
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            int v = *it;
            rank[v] = weight(v);
            for (int w : ops[v].succ) {
                rank[v] = max(rank[v], weight(v) + rank[w]);
                height[v] = max(height[v], height[w] + 1);
            }
        }
        component.resize(n);
        map<int, int> cc;
        for (int v : topo) {
            int r = ds.get(v);
            if (!cc.count(r))
                cc[r] = components++;
            component[v] = cc[r];
        }
        // The iterative predecessor DFS from structure.py, with no recursion-depth risk.
        vector<int> roots(n), position(n), visited(n);
        iota(roots.begin(), roots.end(), 0);
        for (int i = 0; i < n; ++i)
            position[topo[i]] = i;
        sort(roots.begin(), roots.end(), [&](int a, int b) {
            return make_tuple(!ops[a].succ.empty(), -position[a], a) <
                   make_tuple(!ops[b].succ.empty(), -position[b], b);
        });
        vector<pair<int, bool>> st;
        for (int rootv : roots) {
            if (visited[rootv])
                continue;
            st.emplace_back(rootv, false);
            while (!st.empty()) {
                auto [v, expanded] = st.back();
                st.pop_back();
                if (visited[v])
                    continue;
                if (expanded) {
                    visited[v] = 1;
                    dfs_order.push_back(v);
                    continue;
                }
                st.emplace_back(v, true);
                vector<int> p = ops[v].pred;
                sort(p.begin(), p.end(),
                     [&](int a, int b) { return make_pair(rank[a], -a) < make_pair(rank[b], -b); });
                for (int u : p)
                    if (!visited[u])
                        st.emplace_back(u, false);
            }
        }
        dfs_pos.resize(n);
        for (int i = 0; i < n; ++i)
            dfs_pos[dfs_order[i]] = i;
    }
    int size() const { return int(ops.size()); }
    vector<int> normalize_writers(vector<int> assignment) const {
        vector<int> chosen(size(), -1);
        for (int v : topo) {
            int group = writer_group[v];
            if (chosen[group] < 0)
                chosen[group] = assignment[v];
            assignment[v] = chosen[group];
        }
        return assignment;
    }
    double weight(int v) const { return max(1.0, double(ops[v].cycles)); }
};

struct Deadline {
    Clock::time_point start = Clock::now();
    double limit = 3.0;
    bool enabled = true;
    double elapsed() const { return chrono::duration<double>(Clock::now() - start).count(); }
    bool expired() const { return enabled && limit > 0 && elapsed() >= limit; }
    void check() const {
        if (expired())
            throw runtime_error("__deadline__");
    }
};
struct Solution {
    vector<int> core, order;
    string label;
};
inline void validate(const Graph &g, const Solution &s, int k) {
    int n = g.size();
    if ((int)s.core.size() != n || (int)s.order.size() != n)
        throw runtime_error("internal incomplete solution");
    vector<int> pos(n, -1);
    for (int i = 0; i < n; ++i) {
        int v = s.order[i];
        if (v < 0 || v >= n || pos[v] >= 0)
            throw runtime_error("internal repeated op");
        pos[v] = i;
    }
    for (int v = 0; v < n; ++v) {
        if (s.core[v] < 0 || s.core[v] >= k)
            throw runtime_error("internal bad core");
        for (int w : g.ops[v].succ)
            if (pos[v] >= pos[w])
                throw runtime_error("internal non-topological order");
    }
    for (const auto &tensor : g.tensors) {
        if (tensor.producers.size() < 2)
            continue;
        const int writer_core = s.core[tensor.producers.front()];
        for (int writer : tensor.producers)
            if (s.core[writer] != writer_core)
                throw runtime_error("internal tensor " + to_string(tensor.id) +
                                    " has writers on multiple cores");
    }
}
inline string solution_json(const Graph &g, const Solution &s, int k) {
    ostringstream o;
    o << "{\n  \"node_to_subgraph\": {";
    for (int v = 0; v < g.size(); ++v) {
        if (v)
            o << ',';
        o << '\n' << "    \"" << g.ops[v].id << "\": " << g.ops[v].id;
    }
    o << "\n  },\n  \"core_schedules\": [";
    vector<vector<int>> rows(k);
    for (int v : s.order)
        rows[s.core[v]].push_back(v);
    for (int c = 0; c < k; ++c) {
        if (c)
            o << ',';
        o << "\n    [";
        bool first = true;
        for (int v : rows[c]) {
            if (!first)
                o << ", ";
            first = false;
            o << g.ops[v].id;
        }
        o << ']';
    }
    o << "\n  ]\n}\n";
    return o.str();
}

// Direct port of pipeline.py::_Placement: only touched rows/routes are copied in a trial.
struct RouteKey {
    int type, t, src, dst;
    bool operator==(const RouteKey &o) const {
        return tie(type, t, src, dst) == tie(o.type, o.t, o.src, o.dst);
    }
};
struct RouteHash {
    size_t operator()(const RouteKey &x) const {
        uint64_t h = uint32_t(x.t);
        h = (h * 1000003) ^ uint32_t(x.src + 1);
        h = (h * 1000003) ^ uint32_t(x.dst + 1);
        return size_t((h * 17) ^ x.type);
    }
};
struct Proposal {
    int v = -1, core = -1;
    double start = 0, end = 0, bytes = 0;
    vector<pair<int, Row>> rows;
    vector<pair<RouteKey, double>> routes;
};
struct Placement {
    const Graph &g;
    const Config &cfg;
    int k;
    vector<Row> available, fixed;
    vector<double> finish;
    vector<int> core;
    unordered_map<RouteKey, double, RouteHash> route_end;
    double bytes = 0;
    Placement(const Graph &gg, int kk, const Config &c)
        : g(gg), cfg(c), k(kk), available(kk), fixed(kk), finish(g.size(), -1), core(g.size(), -1) {
        route_end.reserve(g.tensors.size() * 2 + 1);
    }
    Proposal trial(int v, int target) const {
        Proposal out;
        out.v = v;
        out.core = target;
        out.rows.reserve(4);
        out.routes.reserve(8);
        auto row = [&](int c) -> Row & {
            for (auto &z : out.rows)
                if (z.first == c)
                    return z.second;
            out.rows.emplace_back(c, available[c]);
            return out.rows.back().second;
        };
        auto copy = [&](RouteKey key, int c, int pipe, i64 size, double release) {
            auto it = route_end.find(key);
            if (it != route_end.end())
                return it->second;
            for (const auto &z : out.routes)
                if (z.first == key)
                    return z.second;
            Row &r = row(c);
            double end = max(r[pipe], release) + max(1.0, ceil(double(size) / cfg.bandwidth));
            r[pipe] = end;
            out.routes.emplace_back(key, end);
            out.bytes += double(size);
            return end;
        };
        auto transfer = [&](int type, int tid, int source, i64 size, double release) {
            double a = copy({type, tid, source, target}, source, 1, size, release);
            return copy({type + 1, tid, source, target}, target, 0, size, a + cfg.delay);
        };
        double ready = 0;
        for (int p : g.ops[v].pred)
            ready = max(ready, finish[p]);
        for (int t : g.ops[v].inputs) {
            const Tensor &x = g.tensors[t];
            if (x.producers.empty()) {
                ready = max(ready, copy({0, t, -1, target}, target, 0, x.size, 0));
                continue;
            }
            vector<pair<int, double>> release;
            for (int p : x.producers) {
                int src = core[p];
                if (src < 0)
                    throw runtime_error("internal unscheduled producer");
                auto it =
                    find_if(release.begin(), release.end(), [&](auto z) { return z.first == src; });
                if (it == release.end())
                    release.emplace_back(src, finish[p]);
                else
                    it->second = max(it->second, finish[p]);
            }
            for (auto [src, r] : release)
                ready = max(ready, src == target ? r : transfer(1, t, src, x.size, r));
        }
        for (int d : g.ops[v].direct_in) {
            const auto &e = g.direct[d];
            if (core[e.a] != target)
                ready = max(ready, transfer(4, d, core[e.a], e.size, finish[e.a]));
        }
        int pipe = g.ops[v].pipe;
        out.start = max(ready, row(target)[pipe]);
        out.end = out.start + g.weight(v);
        row(target)[pipe] = out.end;
        for (int t : g.ops[v].outputs) {
            const auto &x = g.tensors[t];
            if (!x.final_output)
                continue;
            bool complete = true;
            for (int p : x.producers)
                if (p != v && finish[p] < 0)
                    complete = false;
            if (!complete)
                continue;
            vector<pair<int, double>> release;
            for (int p : x.producers) {
                int src = p == v ? target : core[p];
                double r = p == v ? out.end : finish[p];
                auto it =
                    find_if(release.begin(), release.end(), [&](auto z) { return z.first == src; });
                if (it == release.end())
                    release.emplace_back(src, r);
                else
                    it->second = max(it->second, r);
            }
            for (auto [src, r] : release)
                copy({1, t, src, -1}, src, 1, x.size, r);
        }
        return out;
    }
    void commit(const Proposal &p) {
        for (const auto &z : p.rows)
            available[z.first] = z.second;
        for (const auto &z : p.routes)
            route_end[z.first] = z.second;
        finish[p.v] = p.end;
        core[p.v] = p.core;
        fixed[p.core][g.ops[p.v].pipe] += double(g.ops[p.v].cycles);
        bytes += p.bytes;
    }
    double makespan() const {
        double ans = 0;
        for (const Row &r : available)
            for (double x : r)
                ans = max(ans, x);
        return ans;
    }
};

// Serial tensor-lifetime sketch for ORDERING ONLY (not official spill simulation).
struct MemoryOrder {
    const Graph &g;
    const Config &cfg;
    int k;
    vector<int> remaining;
    vector<unsigned char> live;
    vector<array<double, 2>> used;
    MemoryOrder(const Graph &gg, const Config &cc, int kk, const vector<int> &a)
        : g(gg), cfg(cc), k(kk), remaining(g.tensors.size() * size_t(k)), live(remaining.size()),
          used(k) {
        for (int t = 0; t < (int)g.tensors.size(); ++t) {
            vector<int> uses = g.tensors[t].consumers;
            uses.insert(uses.end(), g.tensors[t].producers.begin(), g.tensors[t].producers.end());
            Graph::unique_vec(uses);
            for (int v : uses)
                ++remaining[size_t(t) * k + a[v]];
        }
    }
    double price(int v, int c) const {
        array<double, 2> add{}, release{};
        vector<int> all = g.ops[v].inputs;
        for (int t : g.ops[v].outputs)
            if (find(all.begin(), all.end(), t) == all.end())
                all.push_back(t);
        for (int t : all) {
            size_t key = size_t(t) * k + c;
            if (!live[key])
                add[g.tensors[t].pos] += double(g.tensors[t].size);
        }
        for (int t : all)
            if (remaining[size_t(t) * k + c] == 1)
                release[g.tensors[t].pos] += double(g.tensors[t].size);
        double value = 0;
        for (int m = 0; m < 2; ++m) {
            double limit = double(cfg.spill_limit(m));
            value += add[m] - release[m] + 4 * max(0.0, used[c][m] + add[m] - limit);
        }
        return value / cfg.bandwidth;
    }
    void commit(int v, int c) {
        vector<int> all = g.ops[v].inputs;
        for (int t : g.ops[v].outputs)
            if (find(all.begin(), all.end(), t) == all.end())
                all.push_back(t);
        for (int t : all) {
            size_t key = size_t(t) * k + c;
            if (!live[key]) {
                live[key] = 1;
                used[c][g.tensors[t].pos] += double(g.tensors[t].size);
            }
        }
        for (int t : all)
            --remaining[size_t(t) * k + c];
        for (int t : all) {
            size_t key = size_t(t) * k + c;
            if (!remaining[key] && live[key]) {
                live[key] = 0;
                used[c][g.tensors[t].pos] -= double(g.tensors[t].size);
            }
        }
    }
};
struct SeedOptions {
    string mode = "critical";
    double balance = 0, communication = 0, memory = 0;
    int window = 1;
};
inline Solution operation_plan(const Graph &g, int k, const Config &cfg, const SeedOptions &opt,
                               const vector<int> *assignment, Deadline *deadline) {
    vector<int> normalized;
    if (assignment) {
        normalized = g.normalize_writers(*assignment);
        assignment = &normalized;
    }
    Placement sketch(g, k, cfg);
    int n = g.size();
    vector<int> group_core(n, -1);
    vector<pair<double, double>> priority(n);
    for (int v = 0; v < n; ++v) {
        if (opt.mode == "breadth")
            priority[v] = {g.depth[v], -g.rank[v]};
        else if (opt.mode == "id")
            priority[v] = {double(v), 0};
        else if (opt.mode == "dfs")
            priority[v] = {double(g.dfs_pos[v]), 0};
        else
            priority[v] = {-g.rank[v], 0};
    }
    auto cmp = [&](int a, int b) {
        return make_tuple(priority[a], a) > make_tuple(priority[b], b);
    };
    priority_queue<int, vector<int>, decltype(cmp)> heap(cmp);
    vector<int> degree(n);
    for (int v = 0; v < n; ++v) {
        degree[v] = int(g.ops[v].pred.size());
        if (!degree[v])
            heap.push(v);
    }
    // Avoid constructing a memory model when ownership is not fixed.
    unique_ptr<MemoryOrder> mem;
    if (assignment && opt.memory > 0)
        mem = make_unique<MemoryOrder>(g, cfg, k, *assignment);
    Solution sol;
    sol.core.assign(n, -1);
    sol.order.reserve(n);
    vector<int> candidates;
    while (!heap.empty()) {
        if (deadline && (sol.order.size() % 128 == 0))
            deadline->check();
        candidates.clear();
        for (int i = 0; i < opt.window && !heap.empty(); ++i) {
            candidates.push_back(heap.top());
            heap.pop();
        }
        Proposal chosen;
        tuple<double, pair<double, double>, int> chosen_key{INF, {INF, INF}, 0};
        for (int v : candidates) {
            Proposal best;
            tuple<double, double, double, int> bestkey{INF, INF, INF, 0};
            int p = g.ops[v].pipe;
            for (int c = 0; c < k; ++c) {
                if (assignment && c != (*assignment)[v])
                    continue;
                if (group_core[g.writer_group[v]] >= 0 && c != group_core[g.writer_group[v]])
                    continue;
                Proposal t = sketch.trial(v, c);
                double load = 0;
                for (int j = 0; j < 5; ++j)
                    load = max(load, sketch.fixed[c][j] + (j == p ? g.weight(v) : 0));
                auto key = make_tuple(t.end + opt.balance * load +
                                          opt.communication * t.bytes / cfg.bandwidth,
                                      t.bytes, load, c);
                if (key < bestkey) {
                    bestkey = key;
                    best = move(t);
                }
            }
            double order_score = best.start;
            if (mem)
                order_score += opt.memory * mem->price(v, best.core);
            auto key = make_tuple(order_score, priority[v], v);
            if (key < chosen_key) {
                chosen_key = key;
                chosen = move(best);
            }
        }
        int v = chosen.v;
        for (int x : candidates)
            if (x != v)
                heap.push(x);
        sketch.commit(chosen);
        if (mem)
            mem->commit(v, chosen.core);
        sol.core[v] = chosen.core;
        group_core[g.writer_group[v]] = chosen.core;
        sol.order.push_back(v);
        for (int w : g.ops[v].succ)
            if (!--degree[w])
                heap.push(w);
    }
    validate(g, sol, k);
    return sol;
}

struct TrafficState {
    const Graph &g;
    const Config &cfg;
    int k;
    vector<int> core, pc, cc;
    vector<Row> load;
    double bytes = 0;
    TrafficState(const Graph &gg, const Config &cf, int kk, const vector<int> &a)
        : g(gg), cfg(cf), k(kk), core(a), pc(g.tensors.size() * size_t(k)), cc(pc.size()), load(k) {
        for (int v = 0; v < g.size(); ++v) {
            for (int t : g.ops[v].inputs)
                ++cc[size_t(t) * k + core[v]];
            for (int t : g.ops[v].outputs)
                ++pc[size_t(t) * k + core[v]];
            load[core[v]][g.ops[v].pipe] += double(g.ops[v].cycles);
        }
        for (int t = 0; t < (int)g.tensors.size(); ++t)
            bytes += tensor_bytes(t);
        for (const auto &e : g.direct)
            if (core[e.a] != core[e.b])
                bytes += 2 * double(e.size);
    }
    double tensor_bytes(int t) const {
        int np = 0, nc = 0, intersection = 0;
        for (int c = 0; c < k; ++c) {
            bool p = pc[size_t(t) * k + c] > 0, q = cc[size_t(t) * k + c] > 0;
            np += p;
            nc += q;
            intersection += p && q;
        }
        double copies = np ? 2.0 * (double(np) * nc - intersection) : double(nc);
        if (g.tensors[t].final_output)
            copies += np;
        return copies * double(g.tensors[t].size);
    }
    void move_op(int v, int dest) {
        int src = core[v];
        if (src == dest)
            return;
        vector<int> touched = g.ops[v].inputs;
        touched.insert(touched.end(), g.ops[v].outputs.begin(), g.ops[v].outputs.end());
        Graph::unique_vec(touched);
        for (int t : touched)
            bytes -= tensor_bytes(t);
        for (int d : g.ops[v].direct_inc) {
            const auto &e = g.direct[d];
            if (core[e.a] != core[e.b])
                bytes -= 2 * double(e.size);
        }
        for (int t : g.ops[v].inputs) {
            --cc[size_t(t) * k + src];
            ++cc[size_t(t) * k + dest];
        }
        for (int t : g.ops[v].outputs) {
            --pc[size_t(t) * k + src];
            ++pc[size_t(t) * k + dest];
        }
        load[src][g.ops[v].pipe] -= double(g.ops[v].cycles);
        load[dest][g.ops[v].pipe] += double(g.ops[v].cycles);
        core[v] = dest;
        for (int t : touched)
            bytes += tensor_bytes(t);
        for (int d : g.ops[v].direct_inc) {
            const auto &e = g.direct[d];
            if (core[e.a] != core[e.b])
                bytes += 2 * double(e.size);
        }
    }
    double pipe_bound() const {
        double r = 0;
        for (const auto &l : load)
            for (double x : l)
                r = max(r, x);
        return r;
    }
    double light_score(double alpha) const {
        return max(pipe_bound(), bytes / cfg.bandwidth) + alpha * bytes / cfg.bandwidth;
    }
};

// Bounded-memory reference-string model using farthest-next-use victims.
// This is NOT a replacement for official Step 2: async COPY lifetimes and memory
// reuse edges are not simulated. The estimated spill bytes can differ substantially.
struct SpillEstimate {
    double bytes = 0;
    array<double, 2> peak{};
    bool infeasible = false;
    vector<int> partial_write_victims;
};
inline SpillEstimate estimate_spills(const Graph &g, const Config &cfg, const Solution &s, int k,
                                     Deadline *deadline) {
    const int nt = int(g.tensors.size()), NEVER = numeric_limits<int>::max();
    SpillEstimate ans;
    vector<vector<int>> order(k);
    for (int v : s.order)
        order[s.core[v]].push_back(v);
    struct Event {
        int v;
        vector<pair<int, int>> uses;
    };
    for (int c = 0; c < k; ++c) {
        if (deadline)
            deadline->check();
        if (order[c].empty())
            continue;
        int m = int(order[c].size());
        vector<Event> ev(m);
        vector<int> next(nt, NEVER), last_writer(nt, -1);
        for (int i = 0; i < m; ++i)
            for (int t : g.ops[order[c][i]].outputs)
                last_writer[t] = i;
        for (int i = m - 1; i >= 0; --i) {
            int v = order[c][i];
            ev[i].v = v;
            vector<int> tids = g.ops[v].inputs;
            tids.insert(tids.end(), g.ops[v].outputs.begin(), g.ops[v].outputs.end());
            Graph::unique_vec(tids);
            for (int t : tids) {
                ev[i].uses.emplace_back(t, next[t]);
                next[t] = i;
            }
        }
        vector<unsigned char> resident(nt), seen(nt), backed(nt);
        vector<int> nextuse(nt, NEVER), protect(nt, -1);
        // Equal next-use times follow the oldest active allocation, matching
        // Step 2's stable active-order tie break. A reload gets a fresh stamp.
        using Item = tuple<int, i64, int>;
        array<priority_queue<Item>, 2> heap;
        vector<i64> allocation(nt);
        i64 next_allocation = 0;
        array<double, 2> used{};
        for (int i = 0; i < m; ++i) {
            if (deadline && (i % 512 == 0))
                deadline->check();
            const Event &e = ev[i];
            for (auto [t, nxt] : e.uses)
                protect[t] = i;
            // Allocate all current operands before any last-use release. The
            // soft-limit sweep also runs when this op allocates no new tensor.
            for (auto [t, nxt] : e.uses) {
                (void)nxt;
                if (resident[t])
                    continue;
                const Tensor &x = g.tensors[t];
                int mem = x.pos;
                double sz = double(x.size);
                if (seen[t])
                    ans.bytes += sz;
                else {
                    bool local = false;
                    for (int p : x.producers)
                        if (s.core[p] == c)
                            local = true;
                    backed[t] = !local;
                }
                resident[t] = seen[t] = 1;
                allocation[t] = next_allocation++;
                used[mem] += sz;
            }
            for (int mem = 0; mem < 2; ++mem) {
                vector<Item> held;
                while (used[mem] > double(cfg.spill_limit(mem)) + 0.01) {
                    while (!heap[mem].empty()) {
                        auto [next_step, stamp, b] = heap[mem].top();
                        if (!resident[b] || nextuse[b] != next_step ||
                            -allocation[b] != stamp || next_step == NEVER) {
                            heap[mem].pop();
                            continue;
                        }
                        if (protect[b] == i) {
                            held.push_back(heap[mem].top());
                            heap[mem].pop();
                            continue;
                        }
                        break;
                    }
                    if (heap[mem].empty())
                        break; // Current operands may exceed the soft limit.
                    auto [next_step, stamp, b] = heap[mem].top();
                    (void)next_step;
                    (void)stamp;
                    heap[mem].pop();
                    // Updated Step 2 renames subsequent readers AND writers.
                    // This is a traffic-optimization hint, not infeasibility.
                    if (g.tensors[b].producers.size() > 1 && last_writer[b] >= i)
                        ans.partial_write_victims.push_back(b);
                    resident[b] = 0;
                    used[mem] -= double(g.tensors[b].size);
                    if (!backed[b]) {
                        ans.bytes += double(g.tensors[b].size);
                        backed[b] = 1;
                    }
                }
                for (const auto &item : held)
                    heap[mem].push(item);
                if (used[mem] > double(cfg.capacity[mem]) + 0.01)
                    ans.infeasible = true;
                ans.peak[mem] = max(ans.peak[mem], used[mem]);
            }
            for (int t : g.ops[e.v].outputs)
                backed[t] = 0;
            for (auto [t, nxt] : e.uses) {
                nextuse[t] = nxt;
                int mem = g.tensors[t].pos;
                if (nxt == NEVER) {
                    if (resident[t]) {
                        resident[t] = 0;
                        used[mem] -= double(g.tensors[t].size);
                    }
                } else
                    heap[mem].push({nxt, -allocation[t], t});
            }
        }
    }
    Graph::unique_vec(ans.partial_write_victims);
    return ans;
}
struct Metrics {
    double sketch = 0, route = 0, spill = 0, predicted = 0, score = 0, pipe_bound = 0;
    array<double, 2> peak{};
    bool memory_warning = false;
    vector<int> partial_write_victims;
};
inline Metrics evaluate_proxy(const Graph &g, int k, const Config &cfg, const Solution &s,
                              double alpha, Deadline *deadline) {
    validate(g, s, k);
    Metrics m;
    Placement p(g, k, cfg);
    for (size_t i = 0; i < s.order.size(); ++i) {
        if (deadline && (i % 256 == 0))
            deadline->check();
        int v = s.order[i];
        p.commit(p.trial(v, s.core[v]));
    }
    m.sketch = p.makespan();
    TrafficState tc(g, cfg, k, s.core);
    m.route = tc.bytes;
    m.pipe_bound = tc.pipe_bound();
    auto sp = estimate_spills(g, cfg, s, k, deadline);
    m.spill = sp.bytes;
    m.peak = sp.peak;
    m.memory_warning = sp.infeasible;
    m.partial_write_victims = move(sp.partial_write_victims);
    // A cheap screen only: do not add spill time a second time when DDR is
    // already the limiting resource. Final ranking uses the event refinement.
    m.predicted = max(m.sketch, (m.route + m.spill) / cfg.bandwidth);
    m.score =
        m.predicted + alpha * (m.route + m.spill) / cfg.bandwidth + (sp.infeasible ? 1e18 : 0);
    return m;
}
