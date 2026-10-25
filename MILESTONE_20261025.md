# Memory delegation milestone: md-milestone-20261025

Source marker for the ABI v3/shared-rings memory delegation implementation.
Functional source parent: e1a5cdc9782ba6ae73b72c8396c0ecbf10a36373.
Companion repositories use the same annotated tag: memory-delegation-backend
and llvm-20 (Scudo). Full provenance, benchmark commands/evidence and known limits
are in Backend milestones/20261025/.

Phone reports g8fe53571d89b. Equivalence between that image and this source tree
has not been established. No kernel rebuild, flash, or new phone run accompanies
this marker. Existing sweeps are performance evidence, not full isolation or
exit-session correctness certification. The subsequent 256 MiB policy is not
part of this milestone (the captured user backend uses 2 GiB per CPU).
