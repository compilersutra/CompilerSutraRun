# Timing model (csrun)

This document defines what timing fields mean. It is the contract for the paper
and for CI consumers. See also `experiments/analysis/phase1-audit.md`.

## Modes (paper vocabulary)

| Mode | Meaning | Status |
|------|---------|--------|
| **Mode A — end-to-end** | Host-visible interval covering declared dispatch + required sync | Fallback when device timers unavailable (`cpu_wall`) |
| **Mode B — device interval** | Device/GPU timer queries or events around dispatch | Prefer when available (`gpu_timestamp`) |

Never put Mode A and Mode B medians in one “ranking” column.

## JSON fields

- `timing.source`: `gpu_timestamp` | `cpu_wall` | `unknown`
- `timing.{min,median,mean,max,stddev}_ms`: statistics over **timed iterations only**
  (after warmup), units milliseconds
- Optional `counters.host_*_ms`: separate host bookkeeping (Vulkan), not part of
  the primary median unless Mode A is explicitly defined to include them

## Per-backend vector_add (current implementation)

Transfers (H2D / D2H) are **outside** the timed loop on all backends.

| Backend | Preferred mode | Mechanism |
|---------|----------------|-----------|
| Vulkan | Mode B | `vkCmdWriteTimestamp` span around compute dispatch |
| OpenGL | Mode B | `GL_TIME_ELAPSED` around `DispatchCompute` + barrier + `Finish` |
| HIP | Mode B | `hipEvent` elapsed around launch + `hipDeviceSynchronize` |
| OpenCL | Mode B | Profiling queue + `CL_PROFILING_COMMAND_START/END` on NDRange event; falls back to Mode A (`steady_clock` + `clFinish`) if profiling is unavailable |

## Power (experimental)

When sysfs hwmon GPU nodes exist, `PowerSampler` records samples during the
measured window and may report idle-subtracted active averages. Treat as
optional metadata, not a primary metric, until sampling rate / baseline /
failure modes are fully characterized in the artifact.
