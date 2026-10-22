# Memory Delegation scripts

Scripts are grouped by the workflow they support:

- `camera_continuous_shooting/`: camera Scudo setup and checks, continuous-shot
  pressure benchmarks, and camera allocator-trace replay.
- `kernelsu/`: KernelSU module installation, system/Bilibili/app-switch Scudo
  overlays, and their phone-side checks. Installable module trees are under
  `kernelsu/modules/`.
- The scripts directly under this directory remain the general entry points:
  phone correctness suites, app-switch experiments, SharedArena microbenchmarks,
  and QEMU helpers.
- `phone/` contains phone-side helpers used by the general workflows.

Run the host-side entry points from any working directory; they resolve their
support files relative to their own location.
