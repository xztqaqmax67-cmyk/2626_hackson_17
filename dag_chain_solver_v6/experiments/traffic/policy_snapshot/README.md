# Rejected traffic-policy experiment snapshot

This directory is excluded from CMake. It preserves the source of the H3 probe
whose executable SHA256 is recorded in `manifest.json`, not the shipped solver.
The other algorithm source files are unchanged from base commit
`4ddb75472d248d591fb29cd934e7843b00aa1cfe`.

Policy bits: 1 = useful memory grid; 2 = traffic-aware third refinement slot;
4 = retain a bounded traffic frontier and allow up to 1% estimated cycle cost
for at least 1% estimated total-byte saving. The default options here are all
false. H2 and H3 did not improve the screened outputs and were removed from
production. Their lack of observed gain is not a general impossibility result.

Build from the solver root (add static runtime flags if needed on MinGW):

```sh
g++ -std=c++17 -O2 -pthread -I src experiments/traffic/policy_snapshot/search_probe.cpp experiments/traffic/policy_snapshot/anytime.cpp src/chain.cpp src/refine.cpp src/regions.cpp src/slack.cpp src/tensor_neighborhood.cpp -o policy_probe
```

Invoke directly: `policy_probe graph.json 5 3 5 resume plan.json frozen_seed.json`.
Its usage text still mentions the original 0/1 switch, but the captured source
accepts 0 through 7. The current paired-runner deliberately accepts only 0/1;
historical H2/H3 summaries retain their original policy numbers.

The historical loader assumes singleton identity mapping without explicitly
checking it. All frozen v6.2 plans used in these experiments meet that assumption.
The current development probe adds strict mapping validation.
