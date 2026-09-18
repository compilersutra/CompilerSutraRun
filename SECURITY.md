# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| `main`  | Yes       |

## Reporting a vulnerability

Do **not** open a public issue for security-sensitive reports.

Email **security@compilersutra.com** (or the maintainer listed in `CODEOWNERS`) with:

- Affected commit / tag
- Impact summary
- Minimal reproduction if available

We aim to acknowledge within a few business days.

## Scope notes

`csrun` treats workload YAML and toolchain outputs as untrusted input. Please report path traversal, command injection via toolchain orchestration, and unsafe deserialization issues.
