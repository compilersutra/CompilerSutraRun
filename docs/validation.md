# Validation contract (csrun)

## Predicate (fp32)

An element fails if **both** absolute and relative errors exceed their
tolerances (see `Comparator::compareFloat32`):

\[
|a-b| > \epsilon_{\mathrm{abs}}
\;\wedge\;
\frac{|a-b|}{\max(|b|,10^{-12})} > \epsilon_{\mathrm{rel}}
\]

YAML key `tolerance` currently sets \(\epsilon_{\mathrm{abs}} = \epsilon_{\mathrm{rel}}\).
Optional keys: `absolute_tolerance`, `relative_tolerance`.

## Status values

| Status | Meaning |
|--------|---------|
| `passed` | Comparison ran and matched policy |
| `failed` | Comparison ran and mismatched |
| `skipped` | Validation disabled (`--no-validate` / workload flag) |
| `not_run` | No comparison performed |

A skipped run must **not** be reported as passed.

## Scope today

CPU-reference validation is implemented for `vector_add` via
`VectorAddScaffold`. Other operations (e.g. SPIR-V graphics) may use
different checks or skip numerical comparison.
